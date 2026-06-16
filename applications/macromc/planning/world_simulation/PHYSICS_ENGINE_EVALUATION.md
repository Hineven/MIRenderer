# 物理引擎选型评估
_hineven, 26.6.16_

> 针对 macromc 的 WorldShell + 断裂 + shell 互碰 + 联机需求,评估开源物理引擎。
> 最高权重维度:**mesh-vs-mesh 动态体互碰** 和 **断裂时碰撞体快速重组** —— 这是 voxel+断裂场景的真正瓶颈,也是大部分引擎的硬限制。

## TL;DR

**首选 Jolt Physics,关键妥协是放弃 mesh collider、改用 compound-of-voxel-AABBs。** Rapier 是合理次选但 C++ 集成有税。PhysX 5 的 SDF mesh-mesh 是唯一逃生通道但实验性。

## 对比矩阵

| 维度 | **Jolt** | **Rapier** | **Bullet 3** | **PhysX 5** | **ReactPhysics3D** | **Box2D v3** |
|---|---|---|---|---|---|---|
| 语言/集成 | C++17,原生 | **Rust**,需 FFI | C++ | C++ | C++ | **C17,2D only** |
| Mesh collider 增量更新 | ✅ `MutableCompoundShape` 运行时增删子 shape | ⚠️ collider 重建(trimesh 不可变) | ⚠️ `addChild` + `recalculateLocalAabb`,需移出/移入 world | ⚠️ convex mesh 需重 cook;mesh 几何不可变 | ⚠️ 重建为主 | N/A |
| **动态体 mesh-vs-mesh** | ❌ **硬禁**(MeshShape 不能与另一个 mesh/heightfield 碰撞) | ❌ 不支持(trimesh 仅 static/fixed) | ⚠️ 仅 GImpact,慢且不稳 | 🟡 **PhysX 5 SDF 实验性支持** | ❌ 仅 convex-vs-concave | N/A |
| 大规模 active body | 🟢 优秀,多核线性扩展 | 🟡 中等(~500–1k active @60fps) | 🟡 中等 | 🟢 最强(含 GPU 路径) | 🔴 单线程 | 🟢 2D 内极强 |
| 确定性/同步友好 | 🟡 同二进制+同序调用=确定;不跨平台 | 🟢 **可跨平台确定(需关 SIMD)** | 🔴 默认不确定 | 🔴 无跨平台保证 | 🟡 单线程更易确定 | 🟢 确定,但 2D |
| 许可证 | **MIT** | Apache-2.0 | Zlib | **BSD-3** | Zlib | MIT |
| vcpkg | ✅ `joltphysics` | ❌(Cargo 为主) | ✅ | ✅ | ✅ | ✅ |
| 维护活跃度 | 🟢 极活跃(v5.5.0 ~2025中) | 🟢 活跃(dimforge 全职) | 🔴 **维护模式,官网下线** | 🟢 NVIDIA 持续 | 🟡 小团队慢 | 🟢 活跃 |
| voxel/断裂实战案例 | 🟢 有 | 🟢 **Sable MC mod** | 🟡 老 | 🟡 Omniverse 用 | 🔴 几乎无 | N/A |

## 逐引擎详评

### 1. Jolt Physics — **首选**

**Mesh/compound 动态更新(断裂重组的核心)** —— Jolt 最大优势:
- `StaticCompoundShape`:不可变,build 后只读,查询快。
- **`MutableCompoundShape`**:官方原文 "can be constructed/changed at runtime and trades construction time for runtime performance"。**正是断裂重组需要的** —— shell 断裂后从原 compound 拆出若干 child(每 child 是 chunk-slice 的 box),挂到新 shell 的 MutableCompoundShape,无需整体重建。断裂面局部增删 child,其余复用。
- 注意:MeshShape(triangle mesh)本身**不可变**,断裂时不能用 MeshShape;必须用 compound-of-boxes/convex 才能享受 MutableCompound 增删。

**Shell 间 mesh 互碰 —— Jolt 的硬伤,必须妥协**:MeshShape 文档明确 "cannot collide against other mesh shapes or heightfields",违反触发 assert。
- **Jolt 没有像 PhysX 5 SDF 那样的 mesh-mesh 逃生通道**。
- 绕法:不用 mesh,把 shell 表示为 **compound of AABB boxes**(每 chunk-slice 一个)。box-vs-box 是最廉价碰撞;shell-vs-shell 退化为 compound-vs-compound(Jolt 完全支持);shell-vs-玩家精确(per-voxel AABB)。一箭三雕。

**性能**:有作者(Guerrilla 前 lead)的 multicore scaling 白皮书,与 PhysX 简单场景持平。

**确定性**:同平台 + 同二进制 + API 调用顺序一致 = 确定;**不跨平台**。联机走服务端权威 + 客户端预测/插值,不要 lockstep。Jolt 有 SaveState/RestoreState 支持 predict-rollback。

**许可证/集成**:MIT,最宽松。纯 C++17,CMake,**vcpkg `joltphysics`**,无外部依赖。被 Horizon Forbidden West、Death Stranding 2 使用。

**voxel 实战案例**:
- r/VoxelGameDev 真实失败案例 —— 用户用 Jolt + convex decomposition 做断裂,**因 chunk 数多 + convex decomposition 成本放弃,转去自研 voxel-vs-voxel**。⚠️ **警惕:voxel shell 若用 convex decomposition,500 chunk × 数十 convex 会爆。改用 box-per-chunk-slice 的 compound 可避免此坑**(box 无需 cook、增删 O(1))。
- Discussion #446 官方讨论串。

### 2. Rapier — **次选(C++ 项目的税较重)**

- trimesh collider 不可变,断裂后必须重建 collider。
- trimesh 只能挂 fixed/static body,不支持 dynamic mesh 互碰。绕法同 Jolt。
- 性能 ~500 active body @60fps 是甜蜜点,比 Jolt/PhysX 弱。
- **唯一真正亮点:可跨平台确定,前提是禁用 SIMD**(有专门 `*-deterministic` 构建)。但确定性不跨版本,长期维护负担。
- **最大障碍:Rust 写的,无官方 C++ 绑定**。第三方 `rapier-ffi` 是社区 C ABI 封装,成熟度远不如官方。C++26 + Vulkan 项目走 Rust toolchain + FFI 边界,调试/所有权/性能都要付税。Sable 之所以用 Rapier 是因为它本就是 MC Java mod 外挂的 Rust 进程,边界天然。
- Sable 案例证明 Rapier 能驱动 voxel shell(sub-level = WorldShell),但**只覆盖"shell 移动 + 与静态世界碰撞",没覆盖 shell-vs-shell 大型互推 + 断裂**。

### 3. Bullet Physics — **不推荐**

- `btCompoundShape` 运行时改 child 稳妥流程是 body 移出 world → 改 → 重算 inertia → 移回,断裂频繁时笨重。
- **官网已下线,仓库维护模式,新功能停滞**。对 C++26 新项目选停滞引擎无理由。

### 4. NVIDIA PhysX 5 — **唯一有 SDF 逃生通道,但有代价**

- **唯一原生支持动态 mesh 互碰**(PhysX 5 SDF,Signed Distance Field)。但官方反复标 **experimental**,mesh 单面,有穿透/沉降 bug。
- SDF 需预计算分辨率,断裂后重建 SDF 有成本。
- convex mesh 需 cook,断裂重组体验不如 Jolt 的 MutableCompound。
- 性能最强(可 GPU),但 CPU 路径虽一流、GPU 路径对你浪费。
- **无跨平台确定性**。SDK 体积大、构建重、抽象层厚,集成成本最高。

### 5. ReactPhysics3D — **不推荐(规模/功能不足)**

- **单线程**,对 1000+ shell 直接出局。
- concave 仅 "Static Concave Mesh",无 first-class voxel 支持。
- 性能垫底梯队,适合小项目/教学。

### 6. Box2D v3 — **确认不适用**

- v3.0 用 C17 重写,**严格 2D**,无 3D 支持。直接排除。

## 针对 macromc 的明确推荐

### 推荐:**Jolt Physics** + "compound-of-voxel-AABBs" 策略

**理由**:
1. **断裂重组最适配**:`MutableCompoundShape` 是六引擎里唯一为"运行时增删子 shape"原生设计的 API。断裂 = flood-fill 连通性分析 → child box 集合从原 compound 拆到新 compound,断裂面局部增删。无需重 cook、无需移出 world。
2. **绕开 mesh-vs-mesh 限制代价最低**:每 shell = `MutableCompoundShape{ 一组 AABB box(每 chunk-slice 一个)}`。box-vs-box 最廉价;shell-vs-shell 退化为 compound-vs-compound(完全支持);shell-vs-玩家精确。
3. **集成成本最低**:MIT、纯 C++17、vcpkg、无 Rust、无 NVIDIA 重 SDK。
4. **活跃度最高且对口**:作者做开放世界(Horizon),GDC talk 直接讲大规模物理。
5. **联机走服务端权威**,Jolt 有 SaveState/RestoreState 支持预测回滚。

### 必须接受的主要妥协/风险(诚实指出)

1. **放弃 mesh collider,改用 compound-of-boxes**。这是必须的 —— Jolt MeshShape 既不能动态改又不能互碰,两个核心需求都不满足。compound-of-boxes 在 voxel 场景下**比 convex decomposition 更优**(精确、无需 cook、增删 O(1)),但需要自己写"voxel → chunk-slice AABB 集合"提取逻辑,以及大 shell 的子 shape 数控制(500 chunk = 上万 box 的话,要做 chunk-级合并或 LOD,否则 narrowphase 重)。**这正是 r/VoxelGameDev 失败用户踩的坑 —— 他用 convex decomposition 成本爆炸;用 box 可避免。**
2. **shell 互碰的"面摩擦"精度**:box-vs-box 接触点比连续 mesh 表面粗糙,摩擦行为可能不够细腻。可接受的话最好;若需更高精度,对大 shell 外壳一层用更细的 box 或局部 convex。
3. **不跨平台确定**:4–5 人联机只能服务端权威 + 状态同步,不能 lockstep。对非严格同步需求足够。

### 备选方案(若 Jolt compound 策略验证失败)

- **若 shell 必须用真 mesh 互碰** → 只有 **PhysX 5 + SDF** 一条路,但接受 experimental 标签和集成税。建议先做 Jolt box-compound 原型,确认摩擦/精度不可接受再切。
- **若团队愿付 Rust FFI 税且跨平台确定性是硬需求** → **Rapier**(确定性构建),但性能和断裂重建成本更差。

### 建议的验证原型(2–3 周可证伪)

1. 一个大 shell = 500 个 box 的 MutableCompoundShape,断裂一次测增删耗时。
2. 两个大 shell 互推,测 compound-vs-compound narrowphase 在 ~10k box 对下的帧时间。
3. 4 个 shell 同时断裂,测连通性分析 + compound 重组的总延迟。

这三个测点能直接证伪/证实 Jolt 路线,比读任何文档都准。

## Sources(一手资料)

**Jolt**:
- [JoltPhysics README(GitHub,MIT/C++17/HFW)](https://github.com/jrouwe/JoltPhysics)
- [MeshShape docs — "cannot collide against other mesh shapes"](https://jrouwe.github.io/JoltPhysics/class_mesh_shape.html)
- [Deterministic Simulation 文档](https://jrouwe.github.io/JoltPhysicsDocs/5.0.0/index.html)
- [Discussion #617 跨平台确定性](https://github.com/jrouwe/JoltPhysics/discussions/617)
- [Discussion #1034 predict-rollback / SaveState](https://github.com/jrouwe/JoltPhysics/discussions/1034)
- [Discussion #1001 concave kinematic mesh assert](https://github.com/jrouwe/JoltPhysics/discussions/1001)
- [Discussion #446 voxel 集成](https://github.com/jrouwe/JoltPhysics/discussions/446)
- [GDC 2022 — Architecting Jolt for Horizon Forbidden West](https://www.guerrilla-games.com/read/architecting-jolt-physics-for-horizon-forbidden-west)
- [Jolt Multicore Scaling 白皮书(Jorrit Rouwé)](https://jrouwe.nl/jolt/JoltPhysicsMulticoreScaling.pdf)
- [r/VoxelGameDev — Jolt voxel 断裂真实失败案例](https://www.reddit.com/r/VoxelGameDev/comments/1pmprzv/tips_for_proly-implementing-dynamic-collisions/)

**Rapier**:
- [Rapier Determinism 文档(关 SIMD 才跨平台)](https://rapier.rs/docs/user_guides/rust/determinism/)
- [Colliders 文档(convex decomposition)](https://rapier.rs/docs/user_guides/javascript/colliders/)
- [Sable MC mod(Modrinth)](https://modrinth.com/project/T9PomCSv) / [GitHub ryanhcode/sable](https://github.com/ryanhcode/sable)
- [rapier-ffi(C ABI 第三方绑定)](https://github.com/aecsocket/rapier-ffi)

**Bullet**:
- [Deterministic Bullet(Ubisoft 白皮书,默认不确定)](https://staticctf.ubisoft.com/J3yJr34U2pZ2Ieem48Dwy9uqj5PNUQTn/H25zqlI1hswqqMlGk0JaP/ae9750d16361591b0ccb7406255a6859/DeterministicBullet.pdf)
- [Issue #1737 btCompoundShape 运行时改 child](https://github.com/bulletphysics/bullet3/issues/1737)

**PhysX 5**:
- [PhysX 5 开源(BSD-3,NVIDIA blog)](https://developer.nvidia.com/blog/open-source-simulation-expands-with-nvidia-physx-5-release/)
- [RigidBodyCollision — Dynamic Triangle Meshes with SDFs(实验性)](https://nvidia-omniverse.github.io/PhysX/physx/5.1.0/docs/RigidBodyCollision.html)
- [Issue #571 — PhysX 4 不支持 mesh-mesh,5 实验性支持](https://github.com/NVIDIAGameWorks/PhysX/issues/571)

**ReactPhysics3D**: [GitHub(Zlib,单线程)](https://github.com/DanielChappuis/reactphysics3d)

**Box2D v3**: [Wikipedia(v3.0 C17 重写,2024-08,仅 2D)](https://en.wikipedia.org/wiki/Box2D)
