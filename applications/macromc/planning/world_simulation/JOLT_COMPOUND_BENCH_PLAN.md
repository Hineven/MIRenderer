# Jolt compound-of-boxes 性能实验预案
_hineven, 26.6.16_

> 验证主文档 §6 **阶梯 1**(Sable 路径:多 body + 面剔除 box compound)在 macromc 设想负载下的可行性。
> 本文是**预案**:定义实验目标、负载模型、测点、判定标准。实际实现后回填性能数据。
> 范围:**碰撞 + 断裂重组**(不接 macromc 真实 chunk 数据,纯 Jolt benchmark,但负载参数对齐 DESTINATION)。

## 0. 背景修正(相对初版的关键转向)

初版预案假设"每 shell = 一个 `MutableCompoundShape` 装海量 box(~100k)"。**调研证实此假设架构性不可行**:MutableCompoundShape 无内部加速树(扁平数组,O(N) 线性扫),两个 100k-box shell 互碰 narrowphase 随 N² 爆炸,且 Jolt 作者本人指向自定义 Shape 而非堆大 compound(见 PHYSICS_RESEARCH.md §1)。

**修正方向**:采用 **Sable 验证过的"多 body + 面剔除小 compound"**:
- 每 **chunk-slice**(16³)一个 body,内装几百 box 的小 compound(面剔除后)。
- 大 shell = 一组 chunk-slice body,靠 Jolt broadphase 在 body 层剪枝。
- 这避开了 MutableCompound 无树的坑(小 compound 内 O(N) 无所谓),且 Sable 在 Create Aeronautics 跑千万级方块印证可行。

本预案据此重设计。

## 1. 实验目标

回答一个问题:**阶梯 1(多 body + 面剔除 compound)在 macromc 的设想负载下,碰撞性能和断裂重组性能是否在可接受预算内?**

可接受预算(单机,60fps = 16.6ms/tick,physics 只占其中一部分):
- **稳态碰撞帧时间** < 3ms/帧。
- **断裂突发单次峰值** < 10ms(允许偶发掉帧,不可连续多帧)。
- **碎块堆叠(active body 数膨胀)** 维持 60fps 时 active body 上限 ≥ 500。

**关键不确定性:面剔除后,一个大 shell 产生多少 chunk-slice body,每 body 多少 box。** 这些数决定 broadphase 的 body 对数和每对 narrowphase 成本。Phase 0 先标定。

若阶梯 1 不达标,判定是否升级阶梯 2(自定义 VoxelShape,见主文档 §6.2)。

## 2. 负载模型(对齐 DESTINATION)

### 2.1 Shell 模型

| 参数 | 取值 | 依据 |
|---|---|---|
| 大 shell 体量 | 500 chunk | DESTINATION "500 Chunk 级" |
| **大 shell chunk-slice body 数** | **~8000(500 chunk × 16 slice),待 Phase 0 标定** | 面剔除后非空 slice 数 |
| 每 chunk-slice body 的 box 数 | 几十~几百(面剔除后) | 待 Phase 0 标定 |
| 小 shell box 数 | 5-10 | DESTINATION "多数小" |
| 场景 shell 总数 | 1000+(多数小,少量大) | DESTINATION |
| active body 同时数 | 50-500(dynamic,参与碰撞的) | DESTINATION "4-5 玩家 32×32 ticking" + shell |

### 2.2 程序化 shell 生成(贴近真实建筑/飞船几何)

为了让负载"仿真",shell 不能是规则几何体。采用 **base shape + 随机 grow** 模型,模拟手工搭建的建筑/飞船的不规则形态:

1. **Base shape**:一个紧凑的种子体积(例如 20×8×20 的实心方块基座)。
2. **Grow pass 1(300 次)**:每次随机选外壳一个方块,沿外法线方向生长一个 **5×[1-5]×5** 的块(模拟大块结构扩展,如舱段、楼板)。
3. **Grow pass 2(500 次)**:每次随机选外壳一个方块,生长一个 **[1-5]×1×[1-2]** 的块(模拟细节、桁架、长梁)。
4. 产出:一个 ~500 chunk 量级、不规则、内部有连通空腔的 voxel 集合。
5. 对该集合做**面剔除**(INTERIOR 剔除,见 §2.4),统计 box 数 + body 数。

多组随机种子产出多个不同 shell,避免单一样本偏差。

### 2.3 静态地形

一个大 MeshShape(扁平 16k×16k 三角网格铺地),作为 static body,所有 dynamic 落在上面。这是测点 B 的碰撞对手,也是 shell 内 entity 碰撞的最优路径对照。

### 2.4 面剔除策略(抄 Sable,阶梯 1 核心)

采用 Sable 验证过的面剔除分类(PHYSICS_RESEARCH.md §2.2),不用 mesh collider:

| 分类 | 条件 | 处理 |
|---|---|---|
| INTERIOR | 六轴邻居全固体 | **剔除,不生成 box** |
| FACE | 单面暴露 | 生成单面 box |
| EDGE | bothSides==1 | box |
| CORNER | 非完整立方体/液体/回调方块 | 保守 box |
| EMPTY | 非固体 | 无 |

- 每 chunk-slice 一个 body + 一个小 compound(装该 slice 面剔除后的 box)。
- **box 数变量**:除 L0(逐方块)外,可对 FACE/EDGE box 做 greedy 合并(L2)降低数。
- 对照梯度:**L0 逐方块**(上界)/ **L1 纯面剔除**(Sable 做法)/ **L2 面剔除 + greedy box merge**(macromc 优化)。

#### greedy-box-merge 算法草案(可选优化)

复用现有 `GreedyMesher` 的轴扫描思路,输出 box 而非 quad:
- 对每个轴方向 d(X/Y/Z),在 d 方向逐层扫描,对垂直 d 的 16×16 平面建 mask(同 blockId 且实体)。
- 对 mask 做 2D greedy rectangle merge,每个矩形沿 d 方向延伸成 box,延伸到 blockId 变化为止。
- 输出:`{min_corner, size, block_id}` 列表。

## 3. 实验阶段

### Phase 0:几何标定(body 数 + box 数 + 面剔除算法验证)

**目标**:在跑任何 Jolt 性能测试前,先搞清楚真实大 shell 产生多少 chunk-slice body、每 body 多少 box。这些数决定 broadphase body 对数和 narrowphase 成本。

**步骤**:
1. 实现 §2.2 程序化 shell 生成器(base + 300 次 5×[1-5]×5 grow + 500 次 [1-5]×1×[1-2] grow)。
2. 实现 §2.4 L1 面剔除(INTERIOR/FACE/EDGE/CORNER 分类)。
3. 生成 ≥10 个随机种子的大 shell,各跑 L1 面剔除 + L2 greedy merge。
4. 统计:每 shell 的体素数、非空 chunk-slice body 数、每 body box 数、box 尺寸分布、烘焙耗时。
5. 产出 L0/L1/L2 的 body 数 + box 数对照。

**判定(决定 Phase 1 走向)**:

| L1 实测每 shell body 数 | 含义 | Phase 1 策略 |
|---|---|---|
| < 2000 body | 乐观,broadphase 轻松 | 直接跑 A/B/C,大概率达标 |
| 2000 - 8000 body | 中性,broadphase 可承受 | 跑 A/B/C,关注 broadphase 耗时 |
| **~8000 body(500 chunk × 16 slice 估值)** | **预期值** | **A 重点测 broadphase 剪枝效率** |
| > 8000 body | body 数过多 | 考虑合并相邻 slice 成更大 body |

**Phase 0 产出**:一份 body 数 / box 数标定表,据此修订 §4 测点的参数。

### Phase 1:性能应力测试(见 §4 测点)

Phase 0 标定后,用真实 body/box 数跑 §4 的 A/B/C 测点。

## 4. 测点(三个应力场景 + baseline)

### Baseline:静态地形 + entity 查询吞吐

先建立最优路径的基准,作为对照。
- 地形:128×128 三角网格 MeshShape(static),500 个 capsule entity 走 ray + sphere overlap。
- **测**:1000 次 ray + 1000 次 sphere overlap 的总耗时。
- **判定**:若 baseline 已 > 1ms,说明 entity 查询路径本身要优化,与 compound 无关。

### 测点 A:大 shell 互碰(多 body broadphase + narrowphase)

模拟两个飞船长时间 AABB 重叠推挤。
- 两个大 shell(Phase 0 标定的真实 body 数),各自一组 chunk-slice body,放在重叠位置,施加互推力。
- **变量**:box 粒度(L1 纯面剔除 / L2 面剔除+greedy merge)、重叠体积(10% / 50% / 90%)。
- **测**:稳态下每帧 broadphase(body 对生成)+ narrowphase + contact solver 的耗时分解。
- **判定**:L2 + 50% 重叠下 < 3ms 为达标;若 L2 不达标,L1 是否更差。

> **关键观察点**:多 body 架构下,两个 shell 互碰时 broadphase 应只让"实际 AABB 重叠的 chunk-slice body 对"进 narrowphase。若两 shell 体积重叠 50%,实际 body 对数 ≈ 重叠区的 body 数平方(远小于 8000²)。**这是阶梯 1 能否成立的核心。** 若 broadphase 剪枝后 body 对数仍上万 → 升级阶梯 2。

### 测点 B:碎块堆叠(active body 膨胀 + 接触抖动)

模拟断裂后大量小碎块落在一起。
- 200-500 个小 dynamic shell(各 5-10 box,每 shell 一个 body),从空中落到静态地形,堆成一堆。
- **变量**:碎块数(100/300/500)。
- **测**:active body 数随时间曲线、稳态帧时间、接触点总数、是否有抖动穿透。
- **判定**:500 active body 稳态 < 3ms;观察穿透/抖动严重程度。

### 测点 C:断裂突发(CPU 峰值)

模拟单 tick 内多个大 shell 同时断裂。
- 预热:N 个大 shell 静止(各一组 chunk-slice body)。
- 触发:单帧内对每个 shell 做"连通性分析 + body 重新分组",产出 ~2N 个新 shell(body 重新归属,TRef 改挂载)。
- **变量**:同时断裂数 N(1/4/16)、shell 体量(真实 body 数)。
- **测**:单次断裂操作的耗时分解(连通性分析 vs body 重新归属 vs Jolt body add/remove)。
- **判定**:N=4 + 真实体量 shell 单次 < 10ms 为达标。

## 5. 断裂重组的实现细节(实验内要验证的子问题)

测点 C 里断裂不是"Jolt 求解",是**自定义逻辑 + Jolt body add/remove**,几个子问题要在实验里探明:

1. **连通性分析算法**:对 shell 的 chunk-slice body 集合做 flood-fill / union-find,按 6-邻接判定连通分量。算法复杂度 O(body 数),实测常数。注意 body 数若达 8000,图的构建/遍历成本要计入。
2. **body 重新归属的成本**:断裂后 chunk-slice body 的几何不变,只是从 shell A 的 body-group 搬到 shell B 的 body-group。Jolt body 的 transform 更新(换 shell origin 偏移)+ broadphase 重新插入的成本实测。**多 body 架构下,断裂 = body 重新分组,不是 shape 重建——这是阶梯 1 的核心优势。**
3. **shell transform 烘焙**:每个 chunk-slice body 的 world position = shell transform × body local offset。shell 移动时批量更新所有 body 的 position,脏标记避免静止 shell 每帧重写。
4. **断裂面 detection**:哪些 body 在断面要重算碰撞体?实验简化为"指定断面平面",实际架构里是 gameplay/mark dirty 决定。

## 6. 实验框架(技术实现)

### 6.1 环境

- Jolt v5.5.0+(vcpkg `joltphysics`),Release 构建。
- 独立可执行文件,不依赖 macromc 其他模块(纯 Jolt + glm + 计时)。
- 单机,记录 CPU 型号、核数、频率。

### 6.2 计时手段

- Jolt 内置:`PhysicsSystem::Step` 前后用 `JPH_PROFILE` / 手动 chrono。
- 细分:broadphase、narrowphase、constraint solve、body add/remove 各自计时(Jolt 有对应的 `PhysicsStatCollector` 或类似)。
- 断裂重组部分(custom 逻辑)单独计时,不混入 Jolt Step。

### 6.3 负载生成

- shell 用 §2.2 的程序化生成器(base + grow pass),产出真实形状的 voxel 集合。
- 面剔除用 §2.4 的 INTERIOR/FACE/EDGE/CORNER 分类,产出 L0/L1/L2 多种粒度的 body/box 集。
- 负载生成是 Phase 0 和 Phase 1 共用的共享组件。**面剔除算法本身是 macromc collision 层的核心组件(阶梯 1 直接复用),实验产物可进正式实现。**

### 6.4 输出

每个测点输出:
- 帧时间曲线(前 1000 帧稳态)。
- 关键指标的 min/median/p95/max。
- 与判定标准的对比(达标 / 临界 / 不达标)。
- 瓶颈定位(哪个阶段占比最大)。

## 7. 判定矩阵 & 应急路径

### Phase 0 判定(body 数 + box 数标定)

| L1 实测每 shell body 数 | 判定 | 后续 |
|---|---|---|
| < 2000 | 乐观 | Phase 1 直接跑 |
| 2000 - 8000 | 中性 | Phase 1 跑,关注 broadphase |
| **~8000(预期)** | **基线** | **A 重点测 broadphase 剪枝** |
| > 8000 | body 过多 | 合并相邻 slice 成更大 body |

### Phase 1 判定(性能)

| 测点 | 达标 | 临界 | 不达标 → 下一步 |
|---|---|---|---|
| A 大 shell 互碰 | < 3ms | 3-6ms | **升级阶梯 2(自定义 VoxelShape)**;或 shell 互碰改 coarse AABB + gameplay 事件 |
| B 碎块堆叠 | < 3ms @ 500 body | 抖动明显但稳 | 调 contact persistence / solver 迭代;碎块超阈值后合并成 static |
| C 断裂突发 | < 10ms @ N=4 | 10-30ms | 连通性分析摊到多帧(异步);限制单 tick 断裂数 |
| Baseline entity | < 1ms | 1-2ms | 与 compound 无关,独立优化查询 |

**全局应急**:若 A 不达标,这是阶梯 1 的核心否决点。此时不再纠结 compound 调参,而是:
- **优先**:升级阶梯 2(自定义 VoxelShape,Jolt 作者指的路),保留 Jolt 的 broadphase/solver。
- **备选**:shell 互碰降级为 coarse AABB + gameplay 事件(放弃精确碰撞),只保留 shell 内 entity 碰撞。
- **最后**:评估 PhysX 5 SDF(experimental mesh-mesh)。

## 8. 待回填:实验结果

> 实验完成后,在此节回填实际数据,并据此修订 `WORLD_SIMULATION_PLAN.md` §6。
>
> 模板:
> - Phase 0: [每 shell body 数]、[每 body box 数]、[L0/L1/L2 对照]、[面剔除耗时]
> - 测点 A: [帧时间]、[broadphase pair 数]、[瓶颈]、[达标?]
> - 测点 B: [active body 上限]、[抖动情况]、[达标?]
> - 测点 C: [单次断裂耗时]、[连通性分析占比]、[body 重新归属成本]、[达标?]
> - 结论: [阶梯 1 是否成立 / 需升级阶梯 2 / 需降级 AABB]

## 9. 备注 & 开放子问题

- 面剔除算法(L1 INTERIOR/FACE/EDGE/CORNER)是 macromc collision 层核心组件,阶梯 1 直接复用。
- greedy-box-merge(L2)是可选优化,若 Phase 0 显示 L1 box 数可接受则不急用。
- 本实验**不测联机确定性**(主文档 §7 未决问题,需单独同步实验)。
- 本实验**不测 shell 内 entity 精确碰撞的精度**(entity vs terrain MeshShape,Jolt 原生最优,无需验)。
- 实验数据只代表设想负载;真实 macromc 场景的 shell 几何分布可能更复杂,结论需留安全裕度。
- **测点 A 是阶梯 1 vs 阶梯 2 的判定点。** 多 body 架构下 broadphase 剪枝是关键——若两 shell 50% 重叠时实际 body pair 数仍可控(<几千),阶梯 1 成立;若 broadphase 也救不了,升级阶梯 2。
