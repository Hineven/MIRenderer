# GigaVoxel 光追结构设计（TLAS/BLAS/PTLAS）

> 本文记录 GigaVoxel 接入 ray tracing 的结构设计，覆盖 Renderable 抽象扩容、
> instance customIndex 编码、partition allocator、BLAS build 时机，以及 PTLAS
> (VK_NV_partitioned_acceleration_structure) 的接入路径。
>
> 本文是讨论沉淀；RHI 层 PTLAS 支持暂缓（见 §6），但 renderer 层结构设计先行。

---

## 1. 背景：为什么需要大改

第一轮 GigaVoxel renderer 接入（2026-06-17）让 per-chunk BLAS 能 build，但
`GigaVoxelInstance::GetBLAS()` 返回 null，GigaVoxel **不参与 ray tracing**。

根本原因：mirenderer 的 Renderable 抽象是 **单 renderable → 单 BLAS → 单 TLAS
instance**（`GetBLAS() → AS*`）。GigaVoxel 是 **单 renderable → N chunk BLAS**，
和现有抽象不兼容。

接入 RT 需要：
- Renderable 抽象扩容：单 renderable → N 个 instance 描述。
- renderer 主函数 TLAS gathering 改造：遍历 renderable 的 instance 列表。
- GigaVoxel 产出 N 个 chunk instance（customIndex 编码 shell + chunk）。

---

## 2. PTLAS 语义澄清（关键）

经查证 VK_NV_partitioned_acceleration_structure spec：

- **PTLAS 仍有 instance 层**。`VkPartitionedAccelerationStructureWriteInstanceDataNV`
  就是 instance：含 `transform`、`instanceID`、`instanceMask`、
  `instanceContributionToHitGroupIndex`、`accelerationStructure`(BLAS device address)、
  `partitionIndex`、`explicitAABB`。结构和 `VkAccelerationStructureInstanceKHR`
  几乎一致，多了 `partitionIndex` 和 `explicitAABB`。
- **partition 是 instance 的分组**，不是 BLAS 的分组。每个 instance 归属一个
  partitionIndex。"BLAS 在哪个 partition" = "引用它的 instance 在哪个 partition"。
- **transform 仍在 instance 上**。所以 shader 里 renderable transform 的语义不变，
  **不需要 bake transform 到 BLAS geometry，不需要改顶点，不需要 BLAS build 传 transform_data**。
- **从 ray tracing shader 看，PTLAS 和 TLAS 行为一致**（proposal 原文）。

结论：**"renderable transform = BLAS transform 假设被破坏" 这个担忧不成立。**
transform 一直在 instance 级别。GigaVoxel 的 N 个 chunk instance 各自带 transform
（identity，因为顶点已是世界坐标），归到 partition。

---

## 3. instance customIndex 编码

### 3.1 编码空间

instance customIndex 共 **24 bit**。旧代码用 `renderableIndex | (classIndex << bits)`，
这是 RHI 层还不支持多 hit group 时的遗留——现在 RHI 已支持多 hit group
（SBT instanceContributionToHitGroupIndex），**renderable type 由 SBT hit group
隐式区分，customIndex 的 24bit 全部归该 renderable type 自由使用**。

### 3.2 GigaVoxel 编码

```
[ shellIndex (8 bit) | chunkSlot (16 bit) ]
       低 8 bit              高 16 bit
```

- **shellIndex 8 bit (256)**：GigaVoxelShellRegistry 内的稠密 asset index
  （不是稀疏的 ShellId）。同时活跃的 GigaVoxel asset 不会超过 256（terrain
  shell 通常 1 个，结构物若干）。迷你 shell（单 subchunk）不走 GigaVoxel，
  走独立简单 renderable（见 §7 TODO）。
- **chunkSlot 16 bit (65536)**：asset 内 chunk 的稠密 slot。量级校验：
  - VC 单 shell：~289 chunk（envelope R=8）。
  - VC+TFC 单 shell：~3489，逼近 4096 但 16bit 远超。
  - VC+TFC+LFC 单 shell：~11500，双份（grace）~23000 < 65536。
  - TFC/LFC 上来后用更粗 partition 粒度，BLAS 数反而下降。
- **无 type tag**：GigaVoxel hit group 的 shader 按 `shellIndex|chunkSlot<<8`
  解析。StaticMesh hit group 按自己的编码解析。由 SBT 隐式分派。

### 3.3 ray hit 反查

RT hit 后，shader 从 customIndex 解出 shellIndex + chunkSlot，查
GigaVoxelShellRegistry（SSBO）得到 shell 的 GigaVoxel asset 句柄，
再按 chunkSlot 定位 chunk 的 vertex/index range（GigaVoxelGeometryHeap）。
具体查表结构（instance→shell→chunk→geometry）的 SSBO 布局待 RT 接入时定。

---

## 4. Renderable 抽象扩容

### 4.1 新增结构

```cpp
// 一个 BLAS instance 的完整描述（PTLAS instance 所需全部字段）。
struct RenderableBLASInstance {
    RHIAccelerationStructure* blas;
    glm::mat3x4 transform;
    uint32_t instance_custom_index;
    uint8_t  instance_mask;
    uint32_t instance_contribution_to_hit_group_index;  // SBT offset
    uint32_t partition_index;     // 非 global: allocator 分配; global: 特殊值
    AABB     explicit_aabb;       // 世界空间（PTLAS 要求）
};
```

### 4.2 Renderable 基类新增虚函数

```cpp
// 非 global partition 的 instance 列表。默认空（StaticMesh 等不用）。
virtual std::span<const RenderableBLASInstance> GetPartitionedBLASInstances() const { return {}; }
// global partition 的 instance 列表。默认返回单元素（现有 GetBLAS + transform）。
virtual std::span<const RenderableBLASInstance> GetGlobalBLASInstances() const;
```

- **StaticMesh 等现有 renderable**：覆写 `GetGlobalBLASInstances()` 返回单元素
  （现有 `GetBLAS()` + `GetInstanceCustomIndex()` 逻辑搬过来）。
  `GetPartitionedBLASInstances()` 空。走 global partition（每 instance 独立，
  不占 partitionCount）。
- **GigaVoxel**：覆写 `GetPartitionedBLASInstances()` 返回 N 个 chunk instance。
  `GetGlobalBLASInstances()` 空。

### 4.3 GigaVoxel 的 cached instance 数组

`GetPartitionedBLASInstances()` 返回 `span<const>`，要求 GigaVoxel 维护一个
**持续存在的** `std::vector<RenderableBLASInstance> cached_instances_`，chunk
增删时增量更新。这是 GigaVoxel asset 的新职责。

---

## 5. Partition Allocator（renderer 层）

### 5.1 职责

- `AllocatePartition() → uint32_t`：GigaVoxel chunk load 时调，拿稳定 partitionId。
- `FreePartition(uint32_t)`：chunk unload 时归还。
- 维护 dirty partition set：哪些 partition 本帧需要 rebuild（PTLAS 局部 update）。

### 5.2 全局唯一性约束

- partitionIndex 是 PTLAS 全局的。spec 的 `global partition`
  (`VK_PARTITIONED_ACCELERATION_STRUCTURE_PARTITION_INDEX_GLOBAL_NV`) 是特殊值。
- 非 global partition 跨 renderable **不能重复**：GigaVoxel asset A 用 0..288，
  asset B 从 289 开始。
- allocator 统一调度，保证唯一。

### 5.3 数量上限

- `maxPartitionCount`（属性查询，NVIDIA 一般几千）。
- VC 阶段单 terrain shell ~289 partition，远低于上限。
- 多 GigaVoxel asset 同时 partitioned 的场景（256 × 289 = 73984）理论上超限，
  但**实际同时活跃的 GigaVoxel asset 很少**（近场 terrain 通常 1 个）。VC chunk
  总量会控制。万一超限，还有"chunk 共享 partition"等优化手段（TFC/LFC 上来后
  用更大粒度）。

### 5.4 GigaVoxel 的 chunk→partition 映射

- GigaVoxel asset 持 `map<ChunkId, uint32_t partitionId>`。
- chunk load：`partitionId = allocator->AllocatePartition()`，存入映射。
- chunk unload：`allocator->FreePartition(partitionId)`，擦除映射。
- `GetPartitionedBLASInstances()` 的 partition_index 从映射取。

### 5.5 旧 TLAS 阶段（PTLAS 未接入前）

partition allocator 先**记账**（分配 id、维护映射），旧 TLAS build 忽略 partitionId
（旧 TLAS 本来就支持 N instance，每帧全量 rebuild）。partition 的局部 rebuild
价值等 PTLAS 接入后兑现。VC 几百 instance 的 TLAS 全量 rebuild 很便宜（微秒级）。

---

## 6. BLAS build 时机

### 6.1 renderer 直接碰 RHI（绕 RDG）

renderer 层的 BLAS/TLAS build 是特例：renderer 可以直接碰 RHI
（绕过 RDG，代价是 barrier 自己管）。RHI 的 queue 自己 buffer 指令，
**只要从 render thread 调它就完全保序**。

### 6.2 具体做法

- `GigaVoxelInstance::Update(view, builder)` 内，当 `HasDirtyBLAS()` 时：
  - 直接 `RHI::Get().GetGraphicsCommandQueue()` 拿 queue。
  - `gv->BuildDirtyChunkBLAS_Async(alloc, queue)`（命令录进 queue）。
  - 手动插 `queue.AccelerationStructureBarrier`。
- 这些命令早于本帧 RDG pass 的命令（queue 保序），保证 BLAS build 在 TLAS
  gather 之前完成。
- **不用** `builder.AddPass`（第一轮的做法废弃）。

### 6.3 allocator 访问

`Update` 内取 allocator：`Renderer::Get().GetDeviceAllocator()`（singleton，
RendererView 不持有）。

---

## 7. 暂缓 TODO（本次不实现，记录防忘）

| TODO | 说明 | 触发时机 |
|------|------|---------|
| **迷你 shell renderable** | 单 subchunk 的小 shell（可能成千个）不走 GigaVoxel，新增一个简单 renderable 类型处理（单 BLAS，走 global partition）。 | 迷你 shell 需求出现时 |
| **gameplay WorldShell 持有 chunk 模型** | shell 不仅是 GigaVoxel 的 mesh 数据源——gameplay 侧 WorldShellData 是 ChunkData 的唯一 owner（Phase 1 改造后），同时也是 mesh 重建（S2）的数据源。这个"shell 持有 chunk"的关系在原始 streaming/storage 文档里没体现，需要补完：WorldShellData 的职责 = (1) owning ChunkData 容器 (2) mesh 重建数据源 (3) 对应一个 GigaVoxel asset 的 game-side handle (GigaVoxelShellHandle)。chunk 在 shell 内的生命周期 = worldgen 产物装入 → S2 mesh 消费 → unload 移除。 | 文档完善（持续） |
| **TFC/LFC partition 粒度** | VC per-chunk 一个 partition；TFC/LFC 上来后用更大粒度（region 分组），减少 partition 数 + 降低局部 rebuild 代价。 | TFC/LFC 接入时 |
| **LFC asset 归属** | LFC3/4 覆盖整个 64km 远场，是全局的。LFC 是否和 VC/TFC 同一个 GigaVoxel asset，还是独立 asset？影响单 asset BLAS 数和 chunkSlot 编码压力。 | LFC 接入时 |
| **RHI 层 PTLAS 支持** | 新增 RHI PTLAS 类型 + build API（`vkCmdBuildPartitionedAccelerationStructuresNV` 等）。renderer 绕 RDG 直接用。 | partition 局部 rebuild 价值需要兑现时 |
| **explicitAABB 维护** | PTLAS instance 要 explicitAABB（世界空间）。GigaVoxel 已有 per-chunk AABB；StaticMesh 等也要补。 | PTLAS 接入时 |
| **ray hit 反查 SSBO 布局** | instance→shell→chunk→geometry 的查表 SSBO 结构设计。 | RT shader 接入时 |
| **DLSS/TAA history + LOD 切换** | LOD 转换时的 history invalidation、motion vector。 | LOD 接入时 |
| **Floating Origin** | 64km 精度，camera-relative transform。 | 视距扩展时 |

---

## 8. 本次实施范围（路径 A：旧 TLAS 接入）

目标：per-chunk BLAS 通过 N 个 chunk instance 接入**现有 TLAS**，GigaVoxel
立即能 RT（ray query 能 hit chunk）。partition allocator 先记账。

### 8.1 步骤

1. **Renderable 接口扩容**：基类加 `RenderableBLASInstance` 结构 +
   `GetGlobalBLASInstances` / `GetPartitionedBLASInstances` 虚函数（默认实现满足
   StaticMesh 等）。
2. **StaticMesh 等迁移**：现有 `GetBLAS` 逻辑搬到 `GetGlobalBLASInstances`
   （返回单元素）。`GetBLAS` 保留（兼容，或废弃）。
3. **partition allocator**：renderer 层新增，`AllocatePartition`/`FreePartition`
   + dirty set。先记账。
4. **GigaVoxel cached instances**：asset 持 `cached_instances_` +
   `map<ChunkId, partitionId>`。chunk 增删时增量更新。`GetPartitionedBLASInstances`
   返回 cached span。customIndex 编码 `shellIndex(8)|chunkSlot(16)<<8`。
5. **GigaVoxelInstance::Update 改造**：BLAS build 从 RDG pass 改为 renderer
   直接碰 RHI（`GetGraphicsCommandQueue` + 手动 barrier）。
6. **renderer TLAS gathering 改造**：从"每 renderable 单 instance"改成遍历
   `GetGlobalBLASInstances + GetPartitionedBLASInstances`。旧 TLAS build 接受
   N instance，partition id 暂忽略。
7. **编译验证**：macromc_app + 3d_viewer + streaming_test。

### 8.2 完成标准

- GigaVoxel 参与 TLAS（N 个 chunk instance 进 TLAS）。
- ray query 能 hit chunk BLAS（返回正确的 customIndex）。
- StaticMesh 等现有 renderable 行为不变（走 global partition，单 instance）。
- partition allocator 记账正确（partitionId 分配/回收，暂不用）。
