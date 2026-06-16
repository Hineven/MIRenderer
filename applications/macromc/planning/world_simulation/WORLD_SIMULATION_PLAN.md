# World Simulation 架构总览
_hineven, 26.6.16_

> 本文档定义 macromc 的"游戏逻辑 + 物理 + 流送"层架构,是 `ChunkData` / `WorldShellData` / meshing / physics 的共享基础设计。
> 渲染相关(GigaVoxel LOD、mesh 上传)见 `gigavoxel/`。本文档只管"世界数据如何组织、如何在多线程下被消费"。

## 1. 背景 & 核心矛盾

macromc 的世界由大量 **WorldShell** 组成(见 DESTINATION.md):每个 shell 是一个可独立 transform 的 `transform + chunk 集合` 容器,大 shell 可达 500 chunk,总 shell 数 1000+。一个世界同时要驱动:

- **Gameplay tick**(方块逻辑、红石/温度、实体):权威读写 voxel。
- **Meshing**(greedy mesh + 未来 GigaVoxel LOD):只读 voxel,大规模。
- **Physics**(shell 间刚体碰撞、shell 内实体碰撞、断裂):heavy,可异步。
- **Streaming**(chunk load/unload):结构性变更,~100/tick。

核心矛盾:**这些消费者如果共用一把大锁或一个并发数据结构,竞争会拖垮所有路径。** 本架构的目标是消除热路径上的锁,让每个消费者在自己该跑的阶段里无锁访问。

## 2. 三个关键设计决定(讨论结论)

### 2.1 ChunkData 生命周期由 TRef 管理;Registry 注册/反注册在 tick 内单线程进行

Q1 结论。`ChunkData` 是 `RefCounted<>`,所有消费者用 `TRef<ChunkData>` 续命。**全局 chunk registry 不需要并发安全**——它的结构性变更(增删 chunk、改 active set)只在 **tick 的单线程阶段(S0)** 发生,其他所有阶段都是只读的。

这把"并发 chunk random access"问题彻底降级:不存在并发写 registry,所以 registry 可以是普通 `unordered_map` + 一把普通 mutex(仅在 S0 持有)。消费者拿 chunk 是 TRef copy,零锁。

> 对应 `ContextVoxelSource` 的改造:不再快照裸 `ChunkData*` + 靠 `UnregisterChunk` 取消 task 来保生命周期;改为 mesh task 直接捕获 center + 8 邻居的 `TRef<ChunkData>`。`ChunkMeshingContext::mu_` 可从热路径上移除。

### 2.2 ChunkState 拆成正交的双轴 + hull 派生轴

Q2 结论。现状的 `ChunkState` 单枚举混了 streaming 流水线和 rendering 语义。拆成:

```cpp
// 权威轴:voxel 数据生命周期。streaming 独占。
enum class ChunkPresence : uint8_t {
    kAbsent,      // 不在内存
    kLoading,     // 生成/读盘中
    kReady,       // 数据稳定,可被任意线程读  ← meshing/physics/mesh 的唯一门禁
    kUnloading,
};

// 派生轴:是否参与 tick。gameplay 管理。
enum class ChunkSim : uint8_t {
    kInactive,    // 只 mesh/渲染(render distance 内、sim distance 外)
    kActive,      // 在 active set,参与 S1
    kBorderline,  // 刚进/出 active,邻居圈补齐中
};

// 派生轴:chunk-slice 的 collision(VoxelShape)是否就绪。derivation 管理。
enum class SliceShapeState : uint8_t {
    kDirty,       // voxel 变了,VoxelShape 需重建
    kBuilding,    // S2 正在重建
    kReady,       // committed,可被 S4 physics 读
};
// 派生轴:shell 的 body 分组是否稳定。physics 管理。
enum class ShellBodyState : uint8_t {
    kStable,      // body 分组与 transform 一致,physics 可用
    kRestructuring, // 正在 S2 重组(chunk 归属变 / 断裂)
};
```

核心约束:
- `kActive ⊆ kReady`(没 Ready 不能 tick)。
- **meshing 只看 Presence,不看 Sim** —— 这就是 "render 与 gameplay overlap" 的解耦点:远处一堆 `kReady + kInactive` 的 chunk 照常 mesh,gameplay 完全不碰。
- render distance 与 simulation distance 是两个独立旋钮,不绑死。
- physics(S4)的输入门禁是 `SliceShapeState::kReady && ShellBodyState::kStable`。

原枚举里的 `kMeshBuilding / kReady` 不放进 Presence —— mesh 状态属于 `ChunkMeshingContext::Entry::done`,不污染 chunk 的权威 state。

### 2.3 断裂时 mesh/VoxelShape/body 数据复用:数据跟 chunk 走,不跟 shell 走

核心洞察:**mesh 是 voxel 的纯函数。断裂从 mesh 视角不是"一个 shell 变两个、全部重算",而是"沿断面切断一批 voxel 连接"。** 几何后果只有:
1. 断面那层 subchunk:暴露的新外表面需要 remesh。
2. 远离断面的所有 voxel:邻居关系不变,mesh 原封不动有效。

要让这个复用成立,前提是:**mesh 和 collision 子单元挂在 chunk(或 subchunk)上,不挂在 shell 上。shell 只是 `transform + chunk 集合` 的容器。** 断裂处理退化为:

```
1. 连通性分析(union-find / flood-fill 整个 shell 的 chunk)→ 分量 A、B
2. 新建 shell B,把分量 B 的 chunk 从 A 迁过去(TRef 改归属)
   mesh/collision 子单元跟着 chunk 走,零拷贝零重算
3. 断面 dirty 的 subchunk remesh + 局部 hull rebuild
```

**配套的存储格式决定**:mesh 顶点用 **shell-local 空间**(GigaVoxel 是 renderable-relative,MainWorldShell 除外)。shell 移动/旋转只改一个 mat4,mesh 数据不动。断裂后 chunk 迁到新 shell:顶点在新 shell 的 local 空间重新表达(矩阵变换或 chunk-local 存储),不是重新 mesh。

## 3. 数据分层

三条独立的数据线,三种更新粒度,各管一摊:

| 数据 | 归属粒度 | 内容 | 重建粒度 |
|---|---|---|---|
| **Voxel**(ChunkData) | chunk | 方块 id + state,权威 | S1 checkerboard 写,S2 读 |
| **Mesh**(CPU vertex buffer) | subchunk | greedy mesh 结果,GigaVoxel renderable | subchunk dirty 时 S2 重建 |
| **VoxelShape**(collision) | chunk-slice(16³) | 自定义 Jolt Shape,内部持有 chunk-slice 的 voxel 索引;**跟 chunk 走**,shell 仅持有 body 分组 | dirty slice 时 S2 重建 |
| **Shell body group** | shell | shell 的 Jolt body 集合(transform + VoxelShape 引用);shell 间碰撞靠 broadphase 在 body 层剪枝 | chunk 归属变时 S2 重组 |
| **Events** | 全局/跨 tick | 跨 chunk 超距作用、loose physics 结果回流 | append-only |

**关键不变式:voxel 数据只在 S1 被写,且除了 S2 derivation 读一次之外,绝不流出本 tick。** 这是整套并行的地基。physics 之所以能安全跨 tick,是因为它读 VoxelShape/body 的 committed 快照而不读 voxel —— 它"不住在 voxel 的世界里"。

> **阶梯 2 特有的数据归属**:VoxelShape 挂在 chunk-slice 上(数据跟 chunk 走,§2.3),shell 只持有"body 分组"(transform + VoxelShape 引用的集合)。断裂时 chunk-slice 的 VoxelShape 对象 TRef 续命、原地不动,只是 body 从 shell A 的分组迁到 shell B 的分组——**零几何重建**。只有断面那层 dirty slice 的 VoxelShape 才重算。这是阶梯 2 相比阶梯 1 的关键优势:collision 几何和 shell 归属完全解耦。

## 4. Tick Pacing

> 本节是 macromc 基础设施的**根基设计**。所有上层(gameplay、meshing、physics、streaming)的线程模型和时序契约都从此推导。阶梯 2(自定义 VoxelShape)语义。

### 4.1 Stage 定义

| Stage | 线程模型 | 职责 | 产出 |
|---|---|---|---|
| **S0 Registry** | 单线程 | chunk load/unload、active-set 进出、安装异步生成完的 chunk、处理 shell 注册/反注册请求 | 稳定的 registry + shell 表 |
| **S1 Simulation** | 并行 + checkerboard(2×2 着色) | gameplay 权威 voxel RW、消费上 tick event、发新 event、**tight physics 同步 flush**(ray/box overlap 查 committed VoxelShape) | dirty subchunk/slice 标记 + 新 event |
| **S2 Derivation** | 并行,排干 dirty set | **mesh rebuild**(greedy mesh → GigaVoxel renderable)+ **VoxelShape rebuild**(dirty slice 重建 voxel 索引)+ **body group 重组**(断裂时 chunk 归属迁移) | committed mesh + committed VoxelShape + 稳定 body group |
| **S3 Upload** | render 线程 | mesh → GPU staging | GPU-visible mesh |
| **⊥ boundary** | — | VoxelShape working→committed swap;body group 原子提交 | physics 可读的 committed 快照 |
| **S4 LoosePhysics** | 并行,**跨入 N+1** | 读 committed VoxelShape/body 快照跑 Jolt,产 event 给 N+1 的 S1 | physics event |

**排干语义**:S2 每 tick 把 dirty set 处理完,不跨 tick 积压(除非 dirty 量 burst,见 §5.4)。这是保证 S4 永远读到一份"上一 tick 完整 derivation 结果"的前提。

### 4.2 Pacing Grid

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
| **GPU upload** | — | — | — | **W** | — | — |

### 4.3 安全不变式(整套并行的地基)

> **(I) Voxel 行跨 boundary 全是 `—`。**
> gameplay 在 S1 写的 voxel,除了 S2 derivation 读一次,绝不流出本 tick。S4 loose physics 即使跑到 N+1 的 S1 旁边,它读 committed VoxelShape 不读 voxel → 不可能和 N+1 gameplay 的 voxel 写撕裂。

> **(II) VoxelShape / Body group 的 working 与 committed 是物理隔离的两块内存**,只在 ⊥ boundary 做指针级 swap/commit。working 由 S2 独占写,committed 由 S1/S4 共享读。无锁。

> **(III) S4 只读 `⊥` 之后的 committed 快照**,永远落后 gameplay 一个完整 derivation 周期。physics 的世界(VoxelShape/body)和 gameplay 的世界(voxel)在时间上错开一拍,空间上隔离。

这三条共同保证:**任何两个 stage 之间,同一块数据要么不共存,要么一读一写但分属 working/committed。零细粒度锁,零 snapshot 拷贝。**

### 4.4 Sync gates

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

### 4.5 Checkerboard 与 15 格规则(细化为根基契约)

- **15 格规则**(读深度契约):**单 tick 内即时传播半径 < 一个 chunk 宽(15 < 16)**。保证 tick 的级联永远逃不出 center + 1-ring 已加载集合。它管"读深度"——tick 不需要加载第 2 圈 chunk。跨 chunk 的超距作用必须走 event,下一 tick 生效。
- **Checkerboard**(写隔离契约):2×2 着色,相邻 chunk 不同色、不同 sub-phase tick。保证一个 chunk 被读(邻居 tick 时)时它自己不在被写。消灭 read-while-neighbor-writes 撕裂。
- **两条契约正交**:15 格管"要不要加载更远的 chunk",checkerboard 管"同时 tick 的 chunk 之间安不安全"。缺一不可。

> 工程实现:checkerboard 着色可以预计算(每个 chunk coord 的 color = (x+z) mod 2 的某种扩展)。S1 内按 color 分 2(或 4)个 sub-phase 串行,同 sub-phase 内全并行。active set 不大(~32×32),sub-phase 串行开销可忽略。

### 4.6 Active set 与弱加载邻居圈(§7 未决,此处给工作假设)

- **active set**:以玩家为中心的 simulation distance 内的 chunk,标记 `ChunkSim::kActive`。进出由 S0 处理(玩家移动跨 chunk 边界时)。
- **弱加载邻居圈**:active set 外延 1 圈的 chunk 标记 `kBorderline`,被加载(kReady)但不 tick。它们的存在保证 active set 边缘 chunk 的邻居读取(15 格规则)不 miss。
- **工作假设**:simulation distance 独立于 render distance,通常远小于它(如 sim=8 chunk, render=视 DESTINATION 的 64km)。两者的具体值 §7 待定,但 pacing 设计不依赖具体数值。

## 5. Physics 策略

### 5.1 Tight vs Loose query

- **Tight query**(少):决策当下要结果 → S1 内同步 flush。限定为 ray trace + box overlap 即可兜住,不做完整物理碰撞反馈。几百微秒内出结果。
- **Loose query**(多):下 tick 以 event 回来 → 异步,进 S4。physics 是 heavy 的主体。
- **不造第三套机制**:不要做 async query + 分级 + fallback。fallback 语义难选("没踩到压力板"这种 fallback 会直接导致机关不触发)。

### 5.2 VoxelShape 优先,voxel 作 fallback

shell 内实体碰撞主要走 **committed VoxelShape**(阶梯 2);少量 per-voxel 需求(压力板)通过 AABB proxy 转 VoxelShape 查询。**physics 的世界是 VoxelShape/body 的世界,不是 voxel 的世界。** 这样 physics 和 gameplay 写的根本不是同一份数据,voxel 上的竞态消失(安全不变式 I)。

### 5.3 断裂 case 的完整时序(验证可兜住)

```
Tick N:   S1 gameplay 触发断裂(标记断面 dirty slice + dirty body group)
          S2 启动连通性分析(~0.5ms / 500 chunk)→ 两个分量
              chunk-slice 的 VoxelShape 原地不动(TRef 续命)
              body group 重组:body 从 shell A 迁到 shell B(改归属,零几何重建)
              断面 dirty slice 的 VoxelShape 重算 + dirty subchunk remesh
          ⊥: VoxelShape/body committed swap(新 shell 立即可用)
Tick N+1: S4 physics 用 committed 新 body group 算断裂后运动,产 event
          S1 消费 event(碎片开始移动)
          S2 继续 dirty 队列(背压式,每帧 budget)
Tick N+k: dirty 队列排干
```

断裂 **physics 立即响应**(committed body group 下 tick 就用新分组),**mesh/VoxelShape 渐进补**(玩家看见"裂缝出现→碎块继续 mesh 出来",视觉合理)。卡顿只发生在 body group 重组 + 断面 slice rebuild 的 ms 级,可接受。

**阶梯 2 的关键优势**:collision 几何(VoxelShape)挂在 chunk-slice 上,跟 chunk 走;断裂只是 body 分组迁移,VoxelShape 对象零重建。只有断面那层 dirty slice 才重算。

### 5.4 Derivation 排干策略(初步决定)

mesh rebuild + VoxelShape rebuild + body group 重组,每 tick 排干 dirty set。steady state(无编辑)零成本;断裂/爆炸 burst 时 dirty slice 仍只是局部(断面/爆心一层),排干可兜住。

**VoxelShape rebuild 的成本**:重建一个 16³ chunk-slice 的 VoxelShape(重建 voxel 空间索引)比面剔除烘焙稍重,但单次仍在 μs-ms 级,且只在 dirty slice 上发生。steady state 无 dirty = 零开销。

**留口子**:若未来 burst(玩家传送、大爆炸)dirty 量过大成瓶颈,把 mesh 跨 tick 流水化 + per-subchunk COW(触发条件"写时被读",steady state 零触发)。VoxelShape rebuild 是否也需流水化视实测而定。现阶段不做。

## 6. 物理引擎选型 & 断裂策略

> 状态:**架构定调,初期技术栈 = 阶梯 2(自定义 VoxelShape),实现排期后置。** 当前阶段专注 GigaVoxel 渲染 + tick 安排 + chunk 管理 + 流送基础设施。本节定义物理模块的**工程余量**(数据分层、stage 边界、接口留白),确保日后启动物理实现时直接接入阶梯 2,不需要重构已有系统。
>
> 调研证据见 `PHYSICS_RESEARCH.md`,引擎对比见 `PHYSICS_ENGINE_EVALUATION.md`,性能实验预案见 `JOLT_COMPOUND_BENCH_PLAN.md`。

### 6.1 设计目标与需求档位

macromc 的物理需求经讨论收敛为:
- **飞船不穿过对方**(功能性阻挡)—— 核心需求。
- **面摩擦近似**(detour,不追求精密传动)—— 可接受粗糙。
- **shell 内 entity 精确碰撞**(玩家走在表面上)—— 核心需求。
- **规则/连通性驱动断裂**(gameplay 指定断面或阈值)—— 不做应力。
- **4-5 人联机** —— 服务端权威 + 状态同步。

**明确不做**:精密面摩擦传动、应力场断裂、完整连续介质力学。这些是 DESTINATION 的远期研究项,参照 Sable: True Impact 仍 gamma 级、应力未完成。

### 6.2 物理实现的三个阶梯(阶梯 2 为初期目标,1 为降级备选,3 排除)

讨论得出三个代价递增的实现档位。**初期技术栈选定阶梯 2(自定义 VoxelShape)**——因为它在 collision 几何与 shell 归属解耦上最干净,断裂零几何重建,tick pacing 据此建模(§4)。阶梯 1 保留为"阶梯 2 自研遇阻时的降级备选",阶梯 3 明确排除。

#### 阶梯 1:Sable 路径(多 body + 面剔除 compound)— ~1-2 周,零自研物理

- **碰撞体 = 面剔除 box compound**(抄 Sable 的 INTERIOR/FACE/EDGE/CORNER 分类,见 PHYSICS_RESEARCH.md §2.2)。这是绕开 mesh-vs-mesh 限制的核心。
- **粒度:chunk-slice 级 body**(每 chunk-slice 一个 body + 小 compound,几百 box),**不是**整个 shell 一个巨型 compound。这避开了 MutableCompoundShape 无树 O(N) 的架构性坑。
- 完全用 Jolt 现成功能(broadphase/solver/积分),零自研物理。
- **大 shell 互碰** = 多 body 互碰(chunk-slice body vs chunk-slice body),靠 Jolt broadphase AABB tree 在 body 层剪枝。重叠区只有相邻 chunk-slice 才进 narrowphase。
- **预测**:macromc 的需求档位(不穿透 + 近似摩擦)大概率此阶梯就够。Sable 在 Create Aeronautics 跑千万级方块印证了这条路可行。

#### 阶梯 2:自定义 VoxelShape(Jolt 作者指的路)— ~1.5-3 个月,部分自研

- **触发条件**:阶梯 1 实测不达标(大 shell 互碰 narrowphase 过重)。
- 继承 Jolt `Shape`,内部存 voxel grid + 空间索引,**自己实现 voxel-voxel Collide/CastShape**,接触生成用 voxel 轴对齐法线(免费)。
- 白嫖 Jolt 的 broadphase / solver / 积分 / body 管理。**只自研窄相,不自研引擎。**
- 换来 O(重叠体素数) 而非 O(总 box 数),是 voxel 场景的"正确"复杂度。
- 真实风险:碰撞数学不宽容,微妙 bug 导致穿透/抖动,调试难。需要懂 SAT/GJK/接触生成。
- **这是讨论中确认"值得尝试"的方向,但排到物理实现启动后,凭真实性能数据指导。**

#### 阶梯 3:完整 Teardown 级自研引擎 — 数年,不推荐

- 含自研并行求解器、自研 broadphase、全套断裂物理。
- Dennis Gustafsson 花了数年,他是领域顶级专家。对 macromc 不值得。**明确排除。**

### 6.3 工程余量(现在就要留的接口)

初期技术栈即阶梯 2(自定义 VoxelShape)。以下接口在实现 S0-S4 时预留,确保物理实现启动时直接接入:

1. **ChunkData 与 collision 数据解耦**:VoxelShape 是 chunk-slice 的**派生数据**,挂在 chunk-slice 上,不污染 ChunkData 的权威 voxel。voxel → VoxelShape 的派生由 S2 完成。
2. **S2 Derivation 双产出**:S2 阶段同时负责 mesh rebuild 和 VoxelShape rebuild,两者共享 dirty-subchunk 标记。VoxelShape rebuild = 重建该 chunk-slice 的 voxel 空间索引(自定义 Shape 内部结构)。
3. **SliceShapeState + ShellBodyState 双轴(§2.2 已定义)**:physics(S4)的输入门禁是 `SliceShapeState::kReady && ShellBodyState::kStable`。物理实现启动前,S4 可先空跑。
4. **断裂的 body 分组语义**:断裂 = 连通性分析 → chunk-slice body 重新归属(body 从 shell A 迁到 shell B)。VoxelShape 对象 TRef 续命、零重建;只有断面 dirty slice 重算。接口稳定。
5. **shell transform 烘焙接口**:per-body position 烘焙(shell transform 变了才更新),不依赖具体 Shape 类型(VoxelShape 的 transform 由 Jolt body 携带)。

### 6.4 引擎选型(为阶梯 1/2 锁定)

**Jolt Physics**(MIT、C++17、vcpkg)。理由见 PHYSICS_ENGINE_EVALUATION.md:
- 阶梯 1 用其 broadphase/solver/`MutableCompoundShape`(小 compound,几百 box,无树也无妨)。
- 阶梯 2 用其 `Shape` 继承机制(作者本人在 #446 指向这条路)。
- 集成成本最低(MIT、纯 C++、vcpkg、无 Rust FFI)。
- 联机走服务端权威,Jolt 有 SaveState/RestoreState 支持 predict-rollback。

**明确排除**:Bullet(官网下线、维护模式)、ReactPhysics3D(单线程)、Box2D(2D)、完整自研(阶梯 3)。PhysX 5 SDF 是"若 Jolt 路线彻底失败"的最后备选(experimental mesh-mesh)。

### 6.5 断裂策略

- **断裂判定在 gameplay 层**(物理引擎不算应力)。第一版**规则/连通性驱动**(flood-fill 连通性分析 + gameplay 指定断面或阈值规则);断裂后碎片运动交 Jolt。
- **断裂的数据跟 chunk 走**(§2.3 已定):chunk-slice body 迁移到新 shell 的 body 分组,零几何重建。
- **应力驱动断裂是远期研究**,不在当前排期。
- 破坏判定、断裂、shell 间交互全在 gameplay 层(事件驱动),Jolt 只做 shell 内实体精确碰撞 + 断裂后碎片刚体运动。

### 6.6 性能验证(启动物理实现时第一件事)

> 完整实验预案见 `JOLT_COMPOUND_BENCH_PLAN.md`。

阶梯 1 的可行性需要实测确认。启动物理实现时,第一步是跑实验预案:
- **Phase 0 — 几何标定**:程序化生成真实形状大 shell,面剔除后实测 box 数 / body 数。
- **Phase 1 — 多 body 互碰**:两个大 shell(真实 body 数)50% 重叠互推,测 narrowphase/broadphase/solver 帧时间。< 3ms 达标 → 阶梯 1 成立;不达标 → 升级阶梯 2。

**这个实验本身是阶梯 1 实现的副产品**,不需要额外排期。

## 7. 未决问题

1. **Active set 进出条件**:chunk 进入/退出 ticking 集合的判定(player 移动?固定 sim radius?)和 S0 的咬合 —— 决定"弱加载邻居一圈"的边界。
2. **Event 队列归属**:跨 chunk 事件挂全局还是 per-chunk?影响 S1 并行收割。
3. **应力驱动断裂**(远期):若要做,hull 要扩成带连接强度的图,且大概率只能局部/低频算,需单独立项。
4. **联机物理同步**:4-5 人,server tick 的物理同步策略(lockstep? state replication? Sable/Teardown 的做法待研究)。

## 8. 参考实现 & 资料

- **Sable(ryanhcode)**:MC mod,Rapier + sub-level(= WorldShell)。证明 shell 概念可行;其面剔除 box compound + 增量更新是阶梯 1 的直接参考。https://github.com/ryanhcode/sable
- **Sable: Destructive / True Impact**:Sable 的破坏 addon。回调驱动 + 动能判定,**无连通性碎片化** —— 断裂难度的实证,也是 macromc 需自研的部分。https://modrinth.com/project/PskD6LDU
- **Teardown(Dennis Gustafsson)**:CPU voxel 物理,自研并行求解器。经验宝贵但复现成本极高(阶梯 3,明确排除)。https://blog.voxagon.se/
- **Jolt Physics voxel 集成讨论 #446**:作者 jrouwe 推荐 chunk-slice + StaticCompoundShape,并指向自定义 Shape(阶梯 2)。https://github.com/jrouwe/JoltPhysics/discussions/446
- **本目录其他文档**:`PHYSICS_ENGINE_EVALUATION.md`(引擎对比)、`PHYSICS_RESEARCH.md`(Jolt/Sable 调研证据)、`JOLT_COMPOUND_BENCH_PLAN.md`(性能实验预案)。
