# ChunkRegistry / EventBus 设计
_hineven, 26.6.17_

> 本文档记录 S0 Registry stage 的核心数据结构(`ChunkRegistry` + `EventBus`)的 API 设计与线程契约。是 TICK_PACING §3.1 S0 的实现落地。代码见 `applications/macromc/world/include/world/chunk_registry.h`、`event_bus.h`。

## 1. ChunkState 双轴(拆分动机)

旧 `ChunkState` 是单轴 9 态线性枚举(`kUnloaded → kGenerating → ... → kReady`),纯渲染视角,无法表达"voxel ready 但不 tick"等组合。

**拆为正交双轴**,归属 `ChunkRegistry` 的 Entry(不放在 `ChunkData` 上,避免 chunk 被多 shell 引用时状态歧义):

| 轴 | 枚举 | 含义 | 驱动方 |
|---|---|---|---|
| `ChunkPresence` | kAbsent / kLoading / kReady / kUnloading | voxel 数据生命周期 | S0(注册即步进 + tick Advance) |
| `ChunkSim` | kInactive / kBorderline / kActive | 是否参与 S1 tick | S0(玩家移动跨 chunk 边界) |

`ChunkData` 退化为纯数据容器(Palette + SubChunk[256]),不持状态。

## 2. ChunkRegistry API

```
class ChunkRegistry : public mi::RefCounted<> {
    // 生成器注入(按 ShellCategory,指针非拥有)
    void RegisterGenerator(ShellCategory, ChunkGeneratorFn*);

    // S0 单线程:注册即步进
    void RequestLoad(coord, category, initial_sim);  // kAbsent→kLoading, 启动 worldgen task
    void RequestUnload(coord);                        // →kUnloading, CancelAll, 释放
    void SetSimState(coord, sim);                     // active set 维护

    // S0 单线程:每 tick 一次,收割完成的生成
    vector<ChunkCoord> Advance();                     // kLoading→kReady, install data, 返回新 ready 列表

    // 任意 stage 读(sync A 后稳定)
    TRef<ChunkData> Find(coord);                      // presence==kReady 才返回
    ChunkPresence GetPresence(coord);
    ChunkSim GetSimState(coord);
    ActiveSnapshot GetActiveSnapshot();               // 所有 kActive chunk 的 TRef 快照
    ReadySnapshot GetReadySnapshot();                 // 所有 kReady chunk
};
```

### 2.1 注册即步进 + tick Advance(双层驱动)

- **注册即步进**:`RequestLoad` 内立即 kAbsent→kLoading,创建 `TaskGroup`,在 `TaskGraph` 上 launch worldgen task(`Done(false)` + `BatchEnqueue`,遵循 BatchEnqueue 契约)。task body 在 worker 上 `mi::Create<ChunkData>(coord)` → 调注入的 `ChunkGeneratorFn` 填充 → 写 `shared_ptr<TRef<ChunkData>>` 结果槽 + 置 `done_flag`。
- **tick Advance**:每 tick S0 调一次,遍历所有 kLoading Entry,非阻塞检查 `done_flag`(acquire)。完成的:install ChunkData(TRef 装入 Entry)、kLoading→kReady、应用 pending_sim、加入新 ready 列表。

### 2.2 生成器注入(打破循环依赖)

`ChunkRegistry` 在 world 模块,worldgen 在 worldgen 模块(依赖 world)。为避免 world→worldgen 循环,registry 不直接知道 `Worldgen` 类,而是接受 `ChunkGeneratorFn = std::function<void(coord, ChunkData*)>`。上层 app 把 `Worldgen::GenerateChunk` 适配成这个签名注入。

### 2.3 线程契约

- 所有 mutating 方法(RequestLoad/Unload/SetSimState/Advance):**仅 S0 单线程**。map 本身不加锁,靠 stage 隔离。
- 读方法(Find/GetActiveSnapshot):S1/S2 读已稳定快照(sync A 保证 registry 不再变更)。
- gen task body 在 worker 上,只碰自己捕获的输出(新 `mi::Create<ChunkData>`),绝不碰 registry 的 map。完成信号通过 `Entry::GenHandle`(shared_ptr<atomic<bool>>)回传。

## 3. EventBus API

```
struct Event { ChunkCoord target_chunk; EventPayload payload; };
using EventPayload = std::variant<BlockUpdateEvent, PhysicsContactEvent,
                                  InventoryOpEvent, ChunkLoadedEvent>;

class EventBus : public mi::RefCounted<> {
    bool Emit(const Event& e);                 // 任意线程, 路由到 target 的 region inbox
    vector<Event> DrainAll();                  // S1 开头单线程, 排空所有 region inbox
    void GC(const vector<ChunkCoord>& live);   // S0, 清理无 resident chunk 的 region inbox
};
```

### 3.1 per-region inbox(即时层 / tier 1)

- region = 8×8 chunk(XZ),`ChunkToRegionCoord` 工具(floor 除法)。
- 每 region 一个 `unique_ptr<TConsumeAllQueue<Event>>`(inbox 不可移动——含 `shared_mutex` + 固定数组,所以 `unique_ptr` 包一层让它稳定驻留 map)。
- `TConsumeAllQueue` 是 MPSC:多生产者 `Push`(`shared_lock`)、单消费者 `ConsumeAll`(`unique_lock` 排空)。
- **region residency 寄生 chunk residency**:region inbox 不独立管理生命周期,而是由 `EventBus::GC(live_regions)` 回收——`live_regions` 来自 `ChunkRegistry::GetResidentRegions()`(该 region 内至少有一个 registry entry)。S0 每 tick 调一次 GC,跑在"上一 tick S1 已 DrainAll 排空"的状态上,不会丢活 event。region 不需要独立状态机/tick/sim 状态,纯粹是路由 + 内存分组单位。

### 3.2 双层 event 模型(已决,持久层延后)

跨 tick + 跨 chunk 加载边界存活的语义已确认为**需要**(管道/传送/inventory 不能丢),但分两层落地:

- **tier 1(已实现)= region inbox**:处理**即时 event**——target 在 active set 内、本 tick 就消费。`DrainAll()` 每 tick 排空,无跨 tick 积压。内存靠上述 GC 回收。
- **tier 2(延后)= per-chunk 持久 inbox**:处理**target 不可达的 event**——target 未加载 / 不 sim。tier 1 的 dispatch 发现 target 无消费者时,转投 target chunk 的持久 inbox(挂 registry entry 或 chunk 序列化状态),跨 tick 积压,随 chunk 序列化/反序列化,保证"不吞物体"。

**当前已知限制**(tier 2 落地前):`DrainAll()` 排空后,target 不可达的 event 被 dispatch 直接丢弃。代码里已显式标注(`event_bus.h` DrainAll 注释),等真正接 inventory/管道时实现 tier 2 转投逻辑。

**为何不纯 per-chunk**:region inbox 的价值是 drain 输出按 region 分组、与 checkerboard 调度同构;持久层才需要 per-chunk(序列化归属)。两层各司其职,region 保持轻量。

### 3.2 收割时序(对齐 TICK_PACING §3.2 Events 行)

| 时机 | 操作 | 线程 |
|---|---|---|
| S1 开头(sync A 后) | `DrainAll()` —— 每 region inbox 一次 `ConsumeAll`,扁平化 | 单线程 |
| S1 期间(checkerboard) | 各 chunk `Emit()` 新 event → target 的 region inbox | 多线程并行 |
| S4(跨 tick) | loose physics `Emit()` → 下一 tick S1 drain | 多线程 |

## 4. 已知 TODO(本次骨架未覆盖)

- ~~S0 的 active-set/borderline-ring 自动维护逻辑(玩家移动检测 → `SetSimState` 批量更新)。~~ **已实现**:`MacroMCApp::UpdateStreamingEnvelope()` 在 S0 驱动立方柱 envelope(R=8 active + R+1 borderline),按 camera chunk 的 chebyshev 距离算目标 sim,diff registry 现状后批量 RequestLoad / SetSimState。
- ~~S2 dirty-set 跟踪:`Advance()` 返回的新 ready 列表需标 dirty 驱动 meshing。~~ **已实现**:`MacroMCApp::StageS2_Derivation()` 消费 `dirty_set_`,对每个 dirty chunk 先 `RegisterChunk` 再 `RequestMesh`。S0 把 `Advance()` 的 newly_ready 喂入 dirty_set;新 ready chunk 的已注册邻居也被标 dirty(消除保守边界面的 seam 残留)。
- ~~worldgen → `ChunkGeneratorFn` 的适配层(在 macromc_app 里接 `SimpleTerrainWorldgen`)。~~ **已实现**:`MacroMCApp::InitWorldSubsystems()` 把 `SimpleTerrainWorldgen` / `EmptyWorldgen` 包成 2 参 `ChunkGeneratorFn`(shell 参数传 nullptr,当前 worldgen 不用),按 ShellCategory 注册到 registry。
- event 的 S1 dispatch 路由(`DrainAll` 后按 target_chunk 分发到各 chunk 的 tick handler)。**未实现**(stub:S1 只 DrainAll 不 dispatch)。
- **tier 2 持久 event inbox**(per-chunk,跨加载存活 + 序列化):接 inventory/管道/远距红石时实现。dispatch 转投逻辑替换当前的"target 不可达即丢弃"。**未实现**。
- chunk 磁盘序列化(内存 ↔ 磁盘):当前 unload 即丢弃,重新加载靠 worldgen 确定性重生。**未实现**(worldgen 确定性下可延后)。
- S3 mesh → GPU staging(RDG upload pass)。**未实现**(stub)。

## 5. Streaming 闭环(本轮落地,26.6.17)

三个边的状态(详见下文):

| 边 | 状态 | 实现位置 |
|---|---|---|
| worldgen → 内存 | **通** | `ChunkRegistry::LaunchGeneration` + app 的 `ChunkGeneratorFn` 适配器 |
| 磁盘 ↔ 内存 | **不存在** | 无序列化;worldgen 确定性重生等价,延后 |
| 玩家移动 → sim/streaming | **通** | `ChunkStreamer::Tick(cam_chunk)`(原 `MacroMCApp::UpdateStreamingEnvelope`) |

### 5.1 立方柱 envelope(几何)

- chunk 本身是 16×4096×16 整根 Y 柱,`ChunkCoord` 是 2D(XZ),envelope 在 Y 退化。
- envelope = XZ 平面的 chebyshev 方形:
  - 内层 `cheb <= R` → `kActive`(参与 S1 tick)。R=8 → 17×17 = 289。
  - 外环 `cheb == R+1` → `kBorderline`(resident 但不 tick,供 active 边缘读邻居——15-block 规则)。→ 共 19×19 = 361 resident。
- 玩家位置:暂用 fly camera 的 `view_->camera_.position`(entity 系统起来后再分离)。

### 5.2 10s grace unload(防抖动)

玩家在 chunk 边界来回横跳时,同一 chunk 反复 load/unload。解法:移出 envelope 不立即 unload,进 `pending_unloads_`(coord → 离开时刻)。每 tick:
- 玩家重新进入 → 从 `pending_unloads_` 移除(no-op)。
- 停留超 `kUnloadGraceSeconds`(10s)→ `FlushGraceQueue` 调 `UnregisterChunk`(meshing)+ `RequestUnload`(registry)。

grace 放驱动器层(app),不放 registry——registry 不该知道玩家存在;grace 是"玩家可能回来"的投机策略,只有驱动器知道玩家在哪。与 EventBus 的 10s GC 同构。

### 5.3 dirty-set → meshing(S2 衔接)

- `Advance()` 的 newly_ready → `dirty_set_`;**同时**把新 ready chunk 的已注册邻居也标 dirty(邻居需 re-mesh 以 cull 现在共享的边界面)。
- S2 排空 `dirty_set_`:每个 chunk 先 `RegisterChunk`(给 meshing context 一个 TRef lease,邻居快照能含它)再 `RequestMesh`。
- unload 前(`FlushGraceQueue`)先 `UnregisterChunk` + 从 `dirty_set_` 移除,再 `RequestUnload`。

### 5.4 ChunkCoord 三维遗留根除

历史遗留:`ChunkCoord` 原是 `glm::ivec3`,Y 恒为 0(chunk 整根柱),到处手工塞 `0`——迷惑源。本轮根除为独立 2D struct `{int x, z}`(+ `operator==`),字段名直接表达"XZ 平面坐标,无 Y"。`ChunkCoordHash` 改为只 hash x,z。所有转换函数(`BlockWorldToChunkCoord` / `ChunkCoordToBlockOrigin` / `ChunkToRegionCoord`)去掉手工塞 0。`SubChunkCoord` / `BlockCoord` / `WorldPos` 保持 `ivec3`/`vec3`(它们确实三维)。

### 5.5 streaming 驱动器提取为 `ChunkStreamer`(可测试 + 控制接口地基)

streaming 逻辑原散在 `MacroMCApp` 的 `StageS0`/`StageS2`/`UpdateStreamingEnvelope`/`FlushGraceQueue`,共享 5 个 private 成员,且强耦合 `view_->camera_`——无法 headless 测试,也无法从外部(console/脚本)注入伪玩家位置。

提取为 `ChunkStreamer`(`meshing/chunk_streamer.h`,放 `macromc_meshing` 模块,已依赖 world + meshing,无循环):

```cpp
class ChunkStreamer : public mi::RefCounted<> {
    static mi::TRef<ChunkStreamer> Create(Config);
    void SetRegistry(ChunkRegistry*);            // 非拥有
    void SetMeshingContext(ChunkMeshingContext*); // 非拥有,可选
    void Tick(ChunkCoord cam_chunk);              // S0: envelope diff + grace flush + Advance + dirty harvest
    auto ConsumeDirtySet();                       // S2: 拿走 dirty 集
    // 查询: GetEnvelopeSize / IsInEnvelope / GetPendingUnloadCount / GetDirtySetSize ...
};
```

**关键设计**:
- `Tick(cam_chunk)` 接收外部算好的 chunk 坐标——streamer 不依赖 `RendererView`。这是"控制接口"的 seam:app 从 camera 算,测试从测试序列算,未来的 `mc.set_player_pos` 命令从脚本算。
- 切法 B:streamer 吃下 S0 的 streaming 全部(envelope + grace + Advance + dirty harvest),S2 mesh 派发留在 app(保留 TICK_PACING 的 S0/S2 stage 分离)。
- `ConsumeDirtySet()` 是 S0→S2 的交接面。
- EventBus GC 不进 streamer(EventBus 职责,非 streaming),留在 app 的 S0。

**worldgen 优先级修正**(本轮发现并修复):worldgen task 的距离优先级区间原为 `[kLow, kNormal]`(kNormal≈10亿),远高于 mesh 的 `ComputePriority` 返回值(默认 `0..127`)——worldgen 会抢占 mesh。修正为 `[kLow, 127]`,确保 worldgen 永远 ≤ mesh 基线(mesh 是玩家最终等待的)。

**压力测试**(`tests/macromc/streaming_test.cpp`,5 用例):headless 驱动真实 worldgen + mesh worker:
1. `WalksInLine_ChunksLoadAndUnloadGracefully` — 相机走 20 chunk,envelope 恒定,registry entry 数被 envelope+grace 约束,不爆。
2. `JitterAcrossBorder_NoThrashLoad` — 边界来回抖 20 次,grace 吸收振荡,不 thrash load/unload。
3. `OrphanChunk_KLoadingExitIsTracked` — 回归测试:kLoading chunk 移出 envelope 必须被 envelope diff 捕捉(不被 GetReadySnapshot 漏掉),零 grace 下被 unload。
4. `NewlyReadyMarksNeighboursDirty` — chunk ready 后,已 resident 邻居进 dirty 集。
5. `ConsumeDirtySetDrainsUntilNextHarvest` — 消费后 dirty 集空,直到下次 Advance。

测试用 `WaitForPendingTasks()`(轮询 `GetPendingTaskCount` 到 0)drain worldgen worker——`TaskGraph` 无 `WaitAll`(那是 `TaskGroup` 的方法),`GetPendingTaskCount` 在 cancel task 被 worker pop 后也会递减(task.cpp:345-356),所以轮询安全。
