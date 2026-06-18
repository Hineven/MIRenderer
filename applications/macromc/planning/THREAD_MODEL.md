# MacroMC 线程模型：gameplay / render 分离

> 状态：设计决策已定（2026-06-18 讨论），骨架待实现。
> 关联：`TICK_PACING.md`（stage 契约）、`CHUNK_REGISTRY_DESIGN.md`（registry/streaming）、`PROFILER_DESIGN.md`。

## 0. 为什么要分离

S3 即将从 stub 变成真正的 GPU upload（GigaVoxel）。单线程下 S3 的 GPU upload 会阻塞 gameplay 的固定时间步长（20 TPS）。而且 GigaVoxel 的 LOD streaming 是 render 侧的自主行为（见 §2），它和 gameplay tick 是两种不同频率、不同数据的工作，强行串在一个线程里会互相拖累。

核心原则：**gameplay 线程永远不碰 RHI**。这绕开了 mi/renderer 核心层的所有跨线程风险（RHIResource 非原子 refcount、SPSC 删除队列、owner_thread_ 断言）——因为只有 render 线程碰 RHIResource，那套单线程假设全部成立，不用改。

> 核心层改动评估：**极小**。不需要改 RHIResource refcount / 删除队列 / frame index。
> 唯一可能要动的是 `mi/core/include/core/thr.h` 加一个 `ThreadType::kGameplayThread` 枚举值。
> 真正的改动全在 macromc 层（三线程启动 + 命令队列 + bypass 宏）。

## 1. 三线程模型 + bypass 宏

```
main 线程:     glfw + 窗口 + input 处理 → 投递 input 给 gameplay
gameplay 线程: 固定 20 TPS, S0→S1→S2→boundary→S4, 纯 host 计算
render 线程:   自由 FPS, drain gameplay 命令 + GigaVoxel 自主 streaming/LOD + RenderFrame + AdvanceFrame
```

- glfw 必须在创建 window 的线程（main），无法移走。
- gameplay 和 render 各自独立循环，靠命令队列通信，无锁等待（除队列本身的并发原语）。
- **`MACROMC_BYPASS_RENDER_THREAD`**（CMake 宏，调试用）：开启时三线程合并成单线程——main 线程串行跑 glfw + gameplay tick + render（就是现在的样子）。调试渲染 bug 时回归单线程，避免并发干扰。

> 参考：UE 的 GameThread / RenderThread 分离，以及 `ENQUEUE_RENDER_COMMAND` 的命令 marshaling 模式。

## 2. 两套完全独立的 streaming（核心洞察）

gameplay 的 chunk streaming 和 GigaVoxel 的 LOD streaming 是**两套独立数据、独立范围、独立驱动**，不要混淆。

|  | gameplay streaming | GigaVoxel streaming |
|---|---|---|
| **数据** | ChunkData（精确 voxel，sim 用） | TFC + LFC（渲染专用 LOD：octree node、brick） |
| **范围** | sim envelope（R=8 active） | render distance（远大于 sim） |
| **驱动** | gameplay 线程 → TaskGraph worker worldgen | render 线程 → 同一 TaskGraph worker（低优先级）stream LOD |
| **目的** | gameplay tick（物理/红石/事件） | 画面 |
| **谁管** | gameplay | render |

**关键推论**：
- 远处一个 chunk 在 gameplay 层根本没加载（超出 sim envelope），但 render 的 GigaVoxel 会自己 stream 它的低 LOD 数据来画。
- gameplay chunk unload **不影响** render——render 的 GigaVoxel 数据是独立的，有自己的生命周期（render-distance、fade）。gameplay 只需告知 GigaVoxel"哪些 chunk 更新后渲染数据如何"。
- **LOD stream worker 用同一个 TaskGraph 池子**（不是 render 独立池子），靠优先级区分：LOD stream 低优（远处不急），worldgen/mesh 正常/高优（近处 sim 数据要紧）。空闲核心被 LOD stream 吃掉，忙时让位。复用已有 TaskGraph 优先级机制。

## 3. GigaVoxel renderable：自给自足的渲染实体

- **一个 shell 对应一个 GigaVoxel renderable**。
- renderable **不是 gameplay registry 的镜像**。它是自给自足的渲染实体，自己维护：
  - TFC/LFC cache、page table、brick allocator
  - LOD 选择（按相机距离）
  - visibility culling
  - render-distance streaming（自己委托 worker stream 远处低 LOD）
- gameplay 的 voxel 更新只是 renderable 的**一个数据源**（近处高精度数据来源）；远处低 LOD 由 renderable 自己 stream。

## 4. gameplay → render 通信：命令队列

- **SPSC 命令队列**（gameplay 单生产者，render 单消费者）。
- payload 是 `std::variant`：`UploadChunkMesh`、`DestroyChunkMesh`、`UpdateCamera` 等。
- **移交语义**：gameplay 把 meshing 产出移交给 render，之后不再持有。render 是该数据的唯一持有者（GPU buffer + CPU mirror）。
- `ExecuteOnRenderThread(lambda)` 作为 escape hatch（任意 lambda 排队到 render 线程执行）。
- render 命令按序处理，保证最终一致。render 画"稍微旧一点"的状态完全可接受（最多差一帧）。
- 渲染 bug 时开 bypass 宏回归单线程。

## 5. gameplay → render 的数据形态（per subchunk，移交）

- greedy mesher 产出：per-subchunk 的 mesh 数据（`VoxelMeshJobResult`，256 个 subchunk 槽）。
- 命令 `UploadChunkMesh{shell, coord, VoxelMeshJobResult}` 带 shell category 路由到对应 GigaVoxel renderable。
- 移交后 gameplay 侧 `pending_uploads_` 清掉该 entry。

### ⚠️ TODO（LFC 旁路，现在不做，VC 阶段不用管，但注明免得日后忘）

除了 greedy meshing 的 mesh 数据外，**还要附带一个表面方块 + 颜色列表**给 LFC（Low-frequency cache）渲染用。这是 greedy mesher 的一个**旁路输出**——在 mesh 过程中同时收集每个可见面的 surface block id + 颜色，打包成 LFC 数据，和 mesh 数据一起移交。

当前只关注 VC（voxel color / 可见性缓存），不实现这个旁路。但 greedy mesher 的结构要为这个旁路留口子（产出结构里预留 LFC 字段，或 mesher 接口允许附加输出）。**日后做 LFC streaming 时，这是 greedy_mesher 必须扩展的点。**

## 6. 与 TICK_PACING 的关系

TICK_PACING 的 stage 契约（S0→S1→S2→S3→boundary→S4）在分离后按线程归属重组：

| Stage | 线程 | 说明 |
|---|---|---|
| S0 Registry | gameplay | streamer tick（envelope/grace/advance） |
| S1 Simulation | gameplay | event drain + chunk tick |
| S2 Derivation | gameplay | mesh 派发（worker 跑）→ 产出 host 数据 |
| **boundary** | gameplay→render | gameplay 把本 tick 的渲染命令（UploadChunkMesh 等）推入命令队列 |
| S3 Upload | render | drain 命令队列，在 render 线程创建 GPU 资源 + upload |
| S4 LoosePhysics | gameplay | 跨 tick 物理 |

原来 S3 在 gameplay 线程，现在移到 render 线程。⊥ boundary 从"同线程的 stage 分隔"变成"gameplay→render 的真实跨线程命令投递点"。sync gate A/B/C 在单线程下是空操作（bypass 模式），分离后 boundary 成为真正的同步点（命令队列 drain）。

## 7. 风险与待定点

- **命令队列积压**：gameplay 20 TPS × 每次 N 条命令，render 60 FPS drain。如果 render 帧率掉到 gameplay 以下，队列积压。需要监控（profiler gauge）+ 可能的限流。SPSC ring 满了要处理（丢/等/动态扩容）。
- **bypass 宏的覆盖度**：所有跨线程代码路径都要被 bypass 短路。漏一处，bypass 模式下仍并发 = 失去调试意义。需要一个清晰的抽象（命令队列本身在 bypass 下变成直接执行）。
- ~~**GigaVoxel 的数据入口**~~ **[已确认 2026-06-17]**：mesh 数据移交（路径 A）。`UploadChunkMeshCmd` 携带扁平化后的 `{vector<GigaVoxelVertex>, vector<uint32_t>}`，indices chunk-local。`GigaVoxelVertex` 作为跨层数据契约（renderer 层类型对 game 层可见，符合 mirenderer "下层对上层可见"的分层约定）。LFC/TFC 需要的"表面块 + 颜色旁路"是 macromc mesher 的另一个输出，以后作为另一条命令（非同一 payload）。flatten + 转换在 **render thread** 做（因为 meshing result 只能在 render/main thread 读，`ChunkMeshingContext::chunks_` 单线程），StageS3 直接调 GigaVoxelShellRegistry（不经 SPSC 队列，那是 game→render 专用）。

### 7.1 已实现的结论（2026-06-17 第一轮 GigaVoxel 接入）

第一轮 GigaVoxel renderer 侧接入已完成并编译/回归通过（streaming_test 7/7），核心结论：

- **WorldData 接管 ChunkData 所有权**：WorldShellData 是唯一 owning 容器，ChunkRegistry 改非拥有（`Entry::chunk` 是 `ChunkData*`，`Find` 返回非拥有指针）。worldgen 产物通过 `ChunkInstallSink` 装入 WorldShellData，unload 通过 `ChunkRemoveSink` 清除。meshing 的 owning TRef lease 从 WorldShellData 取（`GetChunk`），不再从 registry。
- **GigaVoxel asset = per-chunk BLAS**：`map<ChunkId, TRef<AS>>` + `blas_dirty_` set。indices chunk-local（无 rebias）。atlas 全局共享（`GigaVoxel::SetGlobalAtlas`）。
- **asset 持 instance**：GigaVoxel 持 `TRef<GigaVoxelInstance>`，instance 持非拥有 `GigaVoxel*` 裸指针（避免 refcount cycle）。
- **BLAS build 时机**：UploadChunk mark dirty → `GigaVoxelInstance::Update` 内 build（第一轮塞进 RDG pass，后续大改改为 renderer 直接碰 RHI——见 GIGAVOXEL_RT_DESIGN.md）。
- **game-side**：`GigaVoxelShellHandle`（per shell，逻辑态，ShellId tag）+ `GigaVoxelShellRegistry`（挂 RenderThreadContext，`map<ShellId, TRef<GigaVoxel>>`）。
- **shell 创建**：显式 `CreateGigaVoxelShellCmd{ShellId, ShellCategory}`，atlas 全局不入命令。
- **暂未接入 TLAS**：第一轮 `GigaVoxelInstance::GetBLAS()` 返回 null，GigaVoxel 不参与 RT。per-chunk BLAS 已 build（验证正确性）但没喂给 TLAS。下一轮 renderer TLAS/BLAS 大改接入（见 GIGAVOXEL_RT_DESIGN.md）。

## 8. 实现顺序建议

1. **本文档**（固化决策）✓
2. **marshaling 骨架**：命令队列 + ExecuteOnRenderThread + bypass 宏 + 三线程启动。命令 payload 先放最小集（UpdateCamera + UploadChunkMesh），不依赖 GigaVoxel 具体结构。
3. **把 S3 从 gameplay 移到 render**：StageS3 的逻辑（drain pending）变成 render 线程 drain 命令队列。
4. **GigaVoxel renderable**：实现 render 侧的 GigaVoxel renderable（shell = 1 个），接收 UploadChunkMesh 命令，建 GPU 资源。此时才确定 §7 的数据入口问题。
5. **GigaVoxel 自主 LOD streaming**：render 委托 worker stream 远处 TFC/LFC。
