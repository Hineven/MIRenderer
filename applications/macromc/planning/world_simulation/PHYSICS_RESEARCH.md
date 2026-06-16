# 物理实现研究备忘录
_hineven, 26.6.16_

> 本文是"Jolt compound-of-boxes voxel 失败案例"与"Sable mod 物理实现"两份调研的结论合并,作为日后启动物理实现时的第一手参考。
> 主文档 §6 给出策略定调(三阶梯),本文给出**为什么这么定**的证据链。

## 1. 调研一:Jolt compound-of-boxes 在大 child 数下的失败证据

### 1.1 源码级确凿结论:MutableCompoundShape 无内部加速树

这是最关键、最确凿的一条(来自 Jolt 头文件源码,非推测):

| | StaticCompoundShape | **MutableCompoundShape** |
|---|---|---|
| 内部结构 | **4 叉 AABB 树** | **无树**,扁平数组,SIMD-4 遍历 |
| 查询复杂度 | O(log₄ N) 剪枝 | **O(N) 线性扫所有 child** |
| 改动成本 | 高(重建树) | 低 |
| 设计意图 | 静态、大体量、建一次 | 频繁增删、child 数适中 |

**含义**:把 ~100k box 塞进 MutableCompoundShape 是反设计意图的。每个 query(含对方 shell 打过来的 collide)都线性扫全部 ~100k child AABB。两个 100k-box 的 MutableCompound 互碰 = 双重线性扫,narrowphase 随 N² 爆炸。**10¹⁰ 的理论 pair 数不会被内部结构救下来——它没有树。**

### 1.2 已知失败/性能案例

| 案例 | 来源 | 教训 |
|---|---|---|
| Reddit r/VoxelGameDev OP 放弃 | [原帖](https://www.reddit.com/r/VoxelGameDev/comments/1pmprzv/) | compound-of-boxes 和 convex decomposition 在海量碎片互碰下都崩;OP 转去自研 voxel-voxel |
| Jolt #446 voxel 集成 | [discussion](https://github.com/jrouwe/JoltPhysics/discussions/446) | **作者 Jorrit 亲自引导 OP 去写自定义 Shape**,而非堆大 compound |
| Godot #105642 | [issue](https://github.com/godotengine/godot/issues/105642) | 上千 box 改 collision shape 极慢(shape 改动开销) |
| godot-jolt #660 | [issue](https://github.com/godot-jolt/godot-jolt/issues/660) | compound 操作单次可达 ~8ms |
| Bullet 论坛 voxel compound | [thread](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=12599) | 跨引擎类比:compound 装海量 voxel 子形状 <1 FPS,建议写 VoxShape |

### 1.3 易误读为"成功"的(实际不适用)

- [Reddit "10k voxel entities @120fps"](https://www.reddit.com/r/VoxelGameDev/comments/16po44e/) —— 这是**多 body、每 body 少量 box**,靠 broadphase 分离。**不是**单 body 内 10k child。**不能用来支持"大 compound"方案,但恰好指向"多 body"方案。**

### 1.4 结论

~100k box / shell 用 MutableCompoundShape **几乎肯定撑不住**,理由是架构性的(O(N) 线性扫),不是调参能解决。StaticCompoundShape(有树)在 voxel 密集重叠时叶子 pair 数仍可能爆炸。

**Jolt 作者本人指向的出路:自定义 VoxelShape**(继承 `Shape`,内部存 voxel grid,自己实现 Collide/CastShape,白嫖 Jolt 的 broadphase/solver/积分)。这正是阶梯 2。

## 2. 调研二:Sable mod 的物理实现(最接近的参考实现)

### 2.1 架构(源码确认 ✅)

- **可插拔后端**:`PhysicsPipeline` 接口 + Provider + 优先级。
- **默认实现 = Rapier(Rust/JNI)**,native `sable_rapier_*.dll`。Rapier 实现类不公开(仅编译分发)。
- **子步进**:`substepTimeStep = 1/20/substeps`。
- **Create contraption(Aeronautics 大船)= sub-level**,不是独立刚体。

### 2.2 碰撞体表示(源码确认 ✅)= 面剔除 box compound

**Sable 不用 mesh collider。** 用面剔除的逐方块 box compound,这是绕开 mesh-vs-mesh 的核心手段:

| 分类 | 条件 | 处理 |
|---|---|---|
| INTERIOR | 六轴邻居全固体 | **剔除,不生成 box** |
| FACE | 单面暴露 | 生成单面 box |
| EDGE | bothSides==1 | box |
| CORNER | 非完整立方体/液体/回调方块 | 保守 box |
| EMPTY | 非固体 | 无 |

- 物理数据按 `BlockState.hashCode()` 缓存(`ConcurrentHashMap`)。
- **增量更新**:区块段级 + 单方块级,非整体重建。

### 2.3 破坏机制(addon 确认 ⚠️)= 回调驱动,无连通性碎片化

- `Sable: Destructive`(官方):"Universal collision callback wired into every physics block" + **真实动能模型(mass × v²)** 逐方块判定是否碎裂。走 JNI 回调。
- `Sable: Collision damage`(第三方,alpha):支持 ship-to-ship ramming。
- **关键**:**Sable 没有"碎片继续当独立刚体"**。源码显示破坏后是整体销毁或逐块碎裂,没有断连图分析、没有多 body 拆分。**这是 macromc 要自研的、Sable 零参考的部分。**

### 2.4 性能:瓶颈不在 Rapier,在 Java 侧同步区块加载

| Issue | 现象 | 根因 |
|---|---|---|
| #1149 | 流体扩散卡 10s+ | `handleBlockChange` → 同步加载邻居区块 |
| #1160 | 大量区块 stall | spark 显示线程同步问题,非 CPU |
| #1162 | 递归崩溃 | `computeIfAbsent` 缓存填充时再触发同缓存查询 |
| #950 | native crash | 方块变更重烘焙时 `narrow_phase.rs` 崩 |

**可靠性**:#402/#390/#1098 —— 大 sub-level 高速移动(>320 格)丢碰撞/进 storage。大壳稳定性本身是开放问题。

### 2.5 未找到的关键证据(诚实标注)

- ❌ **~500 区块大壳互撞的实测性能** —— Sable 上零证据。
- ❌ **碎片化多刚体物理** —— Sable 无此功能。
- ⚠️ Sable 能跑千万级方块,但**未确认是单 body 装海量 box 还是多 body 分离**。从 issue 表现和 Rapier 能力推断,更可能是后者(broadphase 分离的多 body)。

### 2.6 对 macromc 最直接的复用点

**必抄**:
1. 碰撞体 = **面剔除 box compound**(INTERIOR 剔除 + FACE/EDGE/CORNER)—— 绕开 mesh-vs-mesh。
2. **增量更新**(区块段 + 单方块级)。
3. 逐方块物理数据按 block hash 缓存。
4. 子步进积分。

**必避**:
1. 绝不在 collider 烘焙路径同步加载区块(#1149/#1160 的 TPS 杀手)。
2. `computeIfAbsent` 不能递归(#1162)。
3. 大壳坐标/精度管理从第一天就设计(#402/#1098)。

## 3. 两份调研的共同指向

它们从两个方向逼向同一结论:

1. **Jolt 侧**:通用 compound(MutableCompound 尤甚)在大 child 数下架构性不可行。Jolt 作者指向自定义 VoxelShape。
2. **Sable 侧**:最接近的参考实现用"面剔除 box compound"绕开了 mesh-vs-mesh,但回避了碎片化;大壳互撞性能未知。

**共同出路**:
- **短期能落地的**:多 body + 面剔除小 compound(每 chunk-slice 一个 body,几百 box)—— Sable 验证过 + Jolt 验证过("10k entities"帖)。
- **性能不够时的正确升级**:自定义 VoxelShape(voxel-voxel 窄相),白嫖 Jolt 的 broadphase/solver。

两条都指向"**不要用单 body 装海量 box**",这是这次讨论最重要的架构结论。

## 4. Sources

**Jolt**:
- [MutableCompoundShape.h 源码(线性扫描,无树)](https://jrouwe.github.io/JoltPhysicsDocs/5.1.0/_mutable_compound_shape_8h_source.html)
- [StaticCompoundShape.h 源码(4 叉 AABB tree)](https://jrouwe.github.io/JoltPhysicsDocs/5.1.0/_static_compound_shape_8h_source.html)
- [Discussion #446 — Jorrit 建议自定义 shape](https://github.com/jrouwe/JoltPhysics/discussions/446)
- [Discussion #1877 — 5.5 compound 查询优化(但仍是 O(N))](https://github.com/jrouwe/JoltPhysics/discussions/1877)
- [Architecting Jolt Physics — Jorrit Rouwé GDC notes](https://jrouwe.nl/architectingjolt/ArchitectingJoltPhysics_Rouwe_Jorrit_Notes.pdf)
- [Reddit r/VoxelGameDev — 放弃帖](https://www.reddit.com/r/VoxelGameDev/comments/1pmprzv/)
- [Reddit r/VoxelGameDev — 10k entities(多 body,非单 body)](https://www.reddit.com/r/VoxelGameDev/comments/16po44e/)
- [Bullet 论坛 — voxel compound 失败](https://pybullet.org/Bullet/phpBB3/viewtopic.php?t=12599)

**Sable**:
- [GitHub ryanhcode/sable](https://github.com/ryanhcode/sable)
- [Issue #1149 — 同步区块加载卡顿(性能根因)](https://github.com/ryanhcode/sable/issues/1149)
- [Issue #1162 — computeIfAbsent 递归](https://github.com/ryanhcode/sable/issues/1162)
- [Issue #950 — narrow_phase.rs native crash](https://github.com/ryanhcode/sable/issues/950)
- [Issue #830 — destructive JNI 回调钩子](https://github.com/ryanhcode/sable/issues/830)
- [Issue #402/#1098 — 大壳高速移动丢碰撞](https://github.com/ryanhcode/sable/issues/402)
- [Sable: Destructive(CurseForge,官方)](https://www.curseforge.com/minecraft/mc-mods/sable-destructive)

**Teardown(自研参考,代价上限)**:
- [80.lv — Teardown 自研 voxel 物理 ~5ms](https://80.lv/articles/see-what-s-new-in-teardown-creator-s-custom-voxel-physics-engine)
- [Voxagon Blog — Dennis Gustafsson](https://blog.voxagon.se/)
