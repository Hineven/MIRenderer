# Tick Pacing & 数据分层 —— macromc 基础设施脊梁
_hineven, 26.6.16_

> 本文档定义 macromc 所有子系统(data registry / streaming、gameplay tick、physics、rendering)共享的**时序契约(stage 划分 + 数据分层 + sync gate)**。
> **线程模型**(gameplay/render 分离、三线程、bypass 宏、命令队列 marshaling)已独立成文 → 见 `THREAD_MODEL.md`。本文档的 stage 在分离后按线程归属重组(见 THREAD_MODEL §6),但 stage 之间的数据契约不变。
>
> 物理层初期技术栈 = **阶梯 2(自定义 VoxelShape)**,见 `world_simulation/`。
> 本文档只定义"数据如何组织、如何在多线程下被消费",不含各子系统的具体算法。

## 0. 前置概念(供本文独立阅读)

- **WorldShell**:可独立 transform 的 `transform + chunk 集合` 容器。大 shell 可达 500 chunk,总 shell 数 1000+。详见 `world_simulation/WORLD_SIMULATION_PLAN.md` §2。
- **ChunkData**:voxel 数据的权威容器(16×4096×16),`RefCounted<>`,用 `TRef<ChunkData>` 续命。
- **数据跟 chunk 走,不跟 shell 走**:mesh、collision(VoxelShape)的子单元挂在 chunk-slice 上,shell 只是 transform + body 分组容器。断裂时 chunk 迁移,子单元零重建。详见 §2.3。
- **ChunkState 双轴**:`ChunkPresence`(voxel 数据生命周期)+ `ChunkSim`(是否参与 tick)。另有派生轴 `SliceShapeState` / `ShellBodyState`(collision 就绪状态)。
- **VoxelShape**:阶梯 2 的自定义 Jolt Shape,内部持有 chunk-slice 的 voxel 索引。是 voxel 的**派生物**,挂在 chunk-slice 上。

## 1. 核心矛盾

一个世界同时要驱动四个异质消费者:

- **Gameplay tick**(方块逻辑、红石/温度、实体):权威读写 voxel。
- **Meshing**(greedy mesh + GigaVoxel LOD):只读 voxel,大规模。
- **Physics**(shell 间碰撞、shell 内实体碰撞、断裂):heavy,可异步。
- **Streaming**(chunk load/unload):结构性变更,~100/tick。

若共用一把大锁或一个并发数据结构,竞争会拖垮所有路径。**本设计的目标是消除热路径上的锁,让每个消费者在自己该跑的阶段里无锁访问。**

## 2. 数据分层

三条独立的数据线,加上 collision 的两条子线,各管一摊:

| 数据 | 归属粒度 | 内容 | 重建时机 |
|---|---|---|---|
| **Voxel**(ChunkData) | chunk | 方块 id + state,**权威** | S1 checkerboard 写,S2 读 |
| **Mesh**(CPU vertex buffer) | subchunk | greedy mesh 结果,GigaVoxel renderable | subchunk dirty 时 S2 重建 |
| **VoxelShape**(collision) | chunk-slice(16³) | 自定义 Jolt Shape,内部持有 chunk-slice 的 voxel 索引;**跟 chunk 走**,shell 仅持有 body 分组 | dirty slice 时 S2 重建 |
| **Shell body group** | shell | shell 的 Jolt body 集合(transform + VoxelShape 引用);shell 间碰撞靠 broadphase 在 body 层剪枝 | chunk 归属变时 S2 重组 |
| **Events** | 全局/跨 tick | 跨 chunk 超距作用、loose physics 结果回流 | append-only |

**关键不变式:voxel 数据只在 S1 被写,且除了 S2 derivation 读一次之外,绝不流出本 tick。** 这是整套并行的地基。physics 之所以能安全跨 tick,是因为它读 VoxelShape/body 的 committed 快照而不读 voxel —— 它"不住在 voxel 的世界里"。

> **阶梯 2 数据归属**:VoxelShape 挂在 chunk-slice 上(数据跟 chunk 走),shell 只持有"body 分组"(transform + VoxelShape 引用的集合)。断裂时 chunk-slice 的 VoxelShape 对象 TRef 续命、原地不动,只是 body 从 shell A 的分组迁到 shell B 的分组——**零几何重建**。只有断面那层 dirty slice 的 VoxelShape 才重算。collision 几何和 shell 归属完全解耦。

## 3. Tick Pacing

> 本节是 macromc 基础设施的**根基设计**。所有上层(gameplay、meshing、physics、streaming)的线程模型和时序契约都从此推导。

### 3.1 Stage 定义

| Stage | 线程模型 | 职责 | 产出 |
|---|---|---|---|
| **S0 Registry** | 单线程 | chunk load/unload、active-set 进出、安装异步生成完的 chunk、处理 shell 注册/反注册请求 | 稳定的 registry + shell 表 |
| **S1 Simulation** | 并行 + checkerboard(2×2 着色) | gameplay 权威 voxel RW、消费上 tick event、发新 event、**tight physics 同步 flush**(ray/box overlap 查 committed VoxelShape) | dirty subchunk/slice 标记 + 新 event |
| **S2 Derivation** | 并行,排干 dirty set | **mesh rebuild**(greedy mesh → GigaVoxel renderable)+ **VoxelShape rebuild**(dirty slice 重建 voxel 索引)+ **body group 重组**(断裂时 chunk 归属迁移) | committed mesh + committed VoxelShape + 稳定 body group |
| **S3 Upload** | render 线程(分离后;单线程下与 gameplay 同线程) | drain gameplay→render 命令队列(UploadChunkMesh 等),在 render 线程创建 GPU 资源 + upload;清理已 unload chunk 的残留。当前(分离前)实现为轮询 GetResult 入 host pending-upload 队列 | GPU-visible mesh(render 阶段);分离前为 pending-upload 队列 |
| **⊥ boundary** | — | VoxelShape working→committed swap;body group 原子提交 | physics 可读的 committed 快照 |
| **S4 LoosePhysics** | 并行,**跨入 N+1** | 读 committed VoxelShape/body 快照跑 Jolt,产 event 给 N+1 的 S1 | physics event |

**排干语义**:S2 每 tick 把 dirty set 处理完,不跨 tick 积压(除非 dirty 量 burst)。这是保证 S4 永远读到一份"上一 tick 完整 derivation 结果"的前提。

### 3.2 Pacing Grid

行 = 数据,列 = stage,格 = R/W。`—` = 不接触(隔离)。

| 数据 \ Stage | S0 Reg | S1 Sim | S2 Derive | S3 Upload | ⊥ boundary | S4 async(N→N+1) |
|---|---|---|---|---|---|---|
| **Chunk Registry** | **RW**(1-thr) | R | R | R | — | R |
| **Voxel**(ChunkData) | W(install,1-thr) | **RW**(checkerboard) | R | — | — | **—** |
| **Events** | — | **RW**(drain 上tick + emit) | — | — | — | **W**(append) |
| **Mesh**(CPU) | — | — | **W** | R(→GPU) | — | — |
| **VoxelShape — working** | — | — | **W**(rebuild) | — | **swap→** | — |
| **VoxelShape — committed** | — | R(tight phys) | — | — | **←swap** | **R**(loose phys) |
| **Body group — working** | — | — | **W**(重组) | — | **commit→** | — |
| **Body group — committed** | — | R(tight phys) | — | — | **←commit** | **R**(loose phys) |
| **Inventory**(chunk/entity owned) | — | **RW**(owner tick + event) | — | — | — | — |
| **Entity — gameplay state**(AI/交互/生命) | (出生/死亡:W) | **RW** | — | — | — | — |
| **Entity — physics intent**(力/目标速度) | — | **W**(S1 末尾) | — | — | **swap→** | — |
| **Entity — physics state**(Jolt body pos/vel) | — | R(读位置) | — | — | **←swap intent** | **RW**(step) |
| **GPU upload** | — | — | — | **W** | — | — |

### 3.3 安全不变式(整套并行的地基)

> **(I) Voxel 行跨 boundary 全是 `—`。**
> gameplay 在 S1 写的 voxel,除了 S2 derivation 读一次,绝不流出本 tick。S4 loose physics 即使跑到 N+1 的 S1 旁边,它读 committed VoxelShape 不读 voxel → 不可能和 N+1 gameplay 的 voxel 写撕裂。

> **(II) VoxelShape / Body group / Entity-physics 的 working 与 committed 是物理隔离的两块内存**,只在 ⊥ boundary 做指针级 swap/commit。working 由生产方(S2 derivation / S1 末尾)独占写,committed 由消费方(S1 读 / S4 step)共享读。无锁。

> **(III) S4 只读 `⊥` 之后的 committed 快照**,永远落后 gameplay 一个完整 derivation 周期。physics 的世界(VoxelShape/body)和 gameplay 的世界(voxel)在时间上错开一拍,空间上隔离。

这三条共同保证:**任何两个 stage 之间,同一块数据要么不共存,要么一读一写但分属 working/committed。零细粒度锁,零 snapshot 拷贝。**

### 3.4 Sync gates

```
S0 ──sync A──► S1 ──sync B──► S2 ──sync C──► S3 ──⊥──►
                   ▲                             │
                   │          S4 (N's loose phys, 并行进 N+1)
                   └───────── events 回流 ───────┘
```

| Sync gate | 等待什么 | 为什么需要 |
|---|---|---|
| **A**(S0→S1) | registry 变更完成 | S1 并行 tick 需要 chunk 集合 + active set 稳定;checkerboard 着色基于稳定拓扑 |
| **B**(S1→S2) | 所有 voxel 写完 | S2 derivation 要读完整、一致的 voxel 产出 mesh/VoxelShape |
| **C**(S2→S3) | derivation 排干 | mesh 要全部 ready 才能 upload;VoxelShape/body commit 才能让下一拍 S4 用 |

S4 是唯一越界的 runner,与 N+1 各 stage 在网格上逐行验证无写冲突(voxel 不碰、VoxelShape/body 靠双缓冲隔离、events 是 append-only 队列、registry 只读)。

### 3.5 Checkerboard 与 15 格规则(正交契约)

- **15 格规则**(读深度契约):**单 tick 内即时传播半径 < 一个 chunk 宽(15 < 16)**。保证 tick 的级联永远逃不出 center + 1-ring 已加载集合。它管"读深度"——tick 不需要加载第 2 圈 chunk。跨 chunk 的超距作用必须走 event,下一 tick 生效。
- **Checkerboard**(写隔离契约):2×2 着色,相邻 chunk 不同色、不同 sub-phase tick。保证一个 chunk 被读(邻居 tick 时)时它自己不在被写。消灭 read-while-neighbor-writes 撕裂。
- **两条契约正交**:15 格管"要不要加载更远的 chunk",checkerboard 管"同时 tick 的 chunk 之间安不安全"。缺一不可。

> 工程实现:checkerboard 着色可以预计算(每个 chunk coord 的 color = (x+z) mod 2 的某种扩展)。S1 内按 color 分 2(或 4)个 sub-phase 串行,同 sub-phase 内全并行。active set 不大(~32×32),sub-phase 串行开销可忽略。

### 3.6 Active set 与弱加载邻居圈(工作假设)

- **active set**:以玩家为中心的 simulation distance 内的 chunk,标记 `ChunkSim::kActive`。进出由 S0 处理(玩家移动跨 chunk 边界时)。**已落地骨架**:状态归属 `ChunkRegistry` 的 Entry(`ChunkPresence` + `ChunkSim` 双轴),见 `world/include/world/chunk_registry.h`。
- **弱加载邻居圈**:active set 外延 1 圈的 chunk 标记 `kBorderline`,被加载(kReady)但不 tick。它们的存在保证 active set 边缘 chunk 的邻居读取(15 格规则)不 miss。
- **工作假设**:simulation distance 独立于 render distance,通常远小于它(如 sim=8 chunk, render=视 DESTINATION 的 64km)。两者的具体值待定,但 pacing 设计不依赖具体数值。

### 3.6b Event 收割实现(per-region inbox)

**已落地**:`world/include/world/event_bus.h` + `world/include/world/chunk_coord.h` 的 `ChunkToRegionCoord`。

- **归属粒度 = per-region**(1 region = 8×8 chunk,XZ 平面)。每个 region 一个 `TConsumeAllQueue<Event>`(MPSC,`shared_mutex` 保护的批量排空队列)。这是 per-chunk(×N mailbox、散落难 drain)与全局单队列(drain 输出无分组)之间的折中:region 正好是 checkerboard 调度的工作单元,drain 输出天然按 region 分组给 S1 dispatch。
- **收割时序**(对齐 §3.2 Events 行):
  - **S1 开头(sync A 后)**:单线程 `EventBus::DrainAll()`,对每个 region inbox 一次 `ConsumeAll()`,扁平化成 vector。O(总 event 数),单线程,开销可忽略。
  - **S1 期间(checkerboard 并行)**:各 chunk 在 tick 内 `Emit()` 新 event,按 `target_chunk` 落对应 region inbox。`TConsumeAllQueue` 的 MPSC(`shared_lock` Push)支持多线程并发写。
  - **S4(跨 tick)**:loose physics 产 event `Emit()`,落下一 tick S1 drain。
- **Event payload = `std::variant`**(编译期穷尽、类型安全、无堆分配):`BlockUpdateEvent` / `PhysicsContactEvent` / `InventoryOpEvent` / `ChunkLoadedEvent`。新增类型只需加 variant 分支。

### 3.7 Inventory & Entity 的落位

两个模块都不引入新 stage、不引入新并发机制——一个顺承 event 模型,一个顺承 VoxelShape 双缓冲模型。

**Inventory**(纯 gameplay,无物理):
- **归属**:每个 inventory 归一个 owner——要么 chunk 的 block-entity(如箱子),要么 entity(如玩家背包)。
- **并发模型 = event**。跨 owner 的 inventory 操作(玩家操作别人的箱子)不发直接写,而是发 event 给 owner,owner 在自己 S1 tick 时**串行**消费。于是 inventory 写天然单线程(owner tick),无锁。两个玩家抢同一箱子 → 两个 event 排队,owner 顺序处理。
- **落位**:整行只在 S1 有动静(见 grid)。不需要独立 stage。

**Entity**(collision 简单,但 state 要隔离):
- **碰撞体类型分级**:
  - 普通实体(投射物/掉落物/动物,数百):引擎原生 box/capsule。
  - **高级实体(玩家+装备,~10)**:**compound-of-boxes 按骨骼拆分**(box 近似每段肢体,~几十 box/实体)。这个量级在 `MutableCompoundShape` 里完全无压力(MutableCompound 的 O(N) 坑只在大 child 数爆炸,几十 child 无所谓)。
  - 统一性:所有 dynamic collision 体都是 box-based(shell 用自定义 VoxelShape,entity 用 compound-of-boxes)。entity-vs-shell = convex/box vs VoxelShape(Jolt 原生 dispatch);entity-vs-entity = boxes vs boxes(原生)。**没有任何碰撞对落在禁区。**
- **骨骼 box 是 skin-following**:动画每帧驱动 box local transform。数量少(~几百 box),`SetShape`/compound child update 每帧重设是廉价操作,属 S4 physics step 内部常驻开销,不进 grid 的新机制。
- **state 隔离(关键设计点,与数量无关)**:entity 同时被两条线碰——S1 gameplay 读写 AI/生命/交互,S4 physics 读写 position/velocity。规矩:**S1 只读 physics state + 在 S1 末尾写 intent(力/目标速度)到 buffer;S4 读 committed intent、step、更新 body。** 这与 VoxelShape/body group 的双缓冲同构(grid 的 Entity 三行),不引入新机制。
- **entity vs shell 碰撞**:这是 entity 物理的主力(玩家走在飞船上)= convex/box vs VoxelShape。自定义 VoxelShape 的 `CollideShape` 实现里最该优化的一条路径。

## 4. 各子系统的接口位置(谁在哪个 stage 干活)

| 子系统 | 主 stage | 附带 stage | 说明 |
|---|---|---|---|
| **Streaming**(load/unload) | S0 | — | 单线程 registry 变更,~100/tick |
| **Gameplay**(tick) | S1 | — | checkerboard 并行,权威 voxel RW + event |
| **Inventory**(gameplay) | S1 | — | owner 串行 tick + 跨 owner event,无物理 |
| **Meshing**(greedy mesh) | S2 | S3(poll→pending) | 读 S1 稳定 voxel,产出 mesh;S3 轮询 GetResult 入 host pending-upload 队列(原始 VoxelMeshJobResult,延后转换)。GPU upload 留 GigaVoxel 阶段 |
| **Physics — tight** | S1 末尾 | — | ray/box overlap 查 committed VoxelShape,同步 flush |
| **Physics — loose** | S4(跨 tick) | — | 读 committed VoxelShape/body 跑 Jolt,产 event |
| **Derivation**(VoxelShape/body) | S2 | — | 与 meshing 同 stage,共享 dirty 标记 |
| **Entity — gameplay**(AI/交互) | S1 | — | gameplay state RW,S1 末尾写 physics intent |
| **Entity — physics**(Jolt body) | S4(跨 tick) | S1(只读位置) | step physics,intent 经 ⊥ swap |
| **Entity — lifecycle**(出生/死亡) | — | S0 | 注册/反注册进 S0 registry phase |
| **Rendering**(consume) | S3 之后 | — | 消费 GPU mesh + GigaVoxel LOD,纯 render thread |

## 5. 相关文档

- `world_simulation/WORLD_SIMULATION_PLAN.md` —— world simulation 详细设计(state 双轴、断裂策略、物理策略)
- `world_simulation/PHYSICS_RESEARCH.md` —— Jolt/Sable 调研证据
- `world_simulation/PHYSICS_ENGINE_EVALUATION.md` —— 物理引擎选型
- `world_simulation/JOLT_COMPOUND_BENCH_PLAN.md` —— 物理性能实验预案
- **实现**:`world/include/world/chunk_registry.h`(S0 Registry 编排器,双轴状态机 + 注册即步进 + tick Advance)、`world/include/world/event_bus.h`(per-region 收件箱 Event 系统,std::variant payload)、`applications/macromc/src/macromc_app.cpp`(主循环 + S0~S4 stage 骨架)
- `gigavoxel/` —— 渲染层(S2 meshing 产出的消费方)
- `DESTINATION.md` —— 总体愿景(决定负载参数与需求档位)
