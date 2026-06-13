# Task: PTLAS (Partitioned TLAS) Support

**Status**: Planned
**Priority**: P0 (GigaVoxel 基础设施)
**Depends on**: 无
**Estimated Effort**: Large

---

## 1. 背景与动机

### 问题

GigaVoxel 在 64km 视距下可能有 5000~10000+ 个活跃 chunk instances。传统 TLAS 在 instance 数量变化或个别 instance 更新时，需要 rebuild/update 整个 TLAS 数据结构，开销随 instance 数量线性增长。

每帧可能发生：
- 数百个 chunk 加载完成（add instances）
- 数十个 chunk 离开视距（remove instances）
- 数百个 chunk LOD 转换（update transform / BLAS ref）
- 相机移动导致所有 VC instance transform 变化

传统 `vkCmdBuildAccelerationStructuresKHR` 的 update 模式虽然支持 partial update，但性能和灵活性有限，不适合万级 instance 的高频局部更新场景。

### 解决方案

使用 NVIDIA 的 `VK_NV_partitioned_acceleration_structure` 扩展（PTLAS）。

PTLAS 将 TLAS 分成多个 **partition**，每个 partition 可以独立 update/rebuild，不触发整个 TLAS 的重建。这天然适合 GigaVoxel 的场景：
- 按空间区域（如 chunk grid tile）或按 LOD 级别分组为不同 partition
- 只有发生变化的 partition 需要 rebuild
- 新增/移除 chunk 只影响对应 partition
- 支持更高 instance 数量（>100K，参见 NVIDIA 100K domino demo）

---

## 2. Vulkan 扩展概要

**Extension**: `VK_NV_partitioned_acceleration_structure` (ext #571)
**Requires**: `VK_KHR_acceleration_structure`
**Platform**: NVIDIA RTX only (符合项目要求)

### 新增 API

| API | 用途 |
|-----|------|
| `vkGetPartitionedAccelerationStructuresBuildSizesNV` | 查询 build 所需内存 |
| `vkCmdBuildPartitionedAccelerationStructuresNV` | 构建/更新 PTLAS |

### 关键数据结构

| 结构 | 用途 |
|------|------|
| `VkBuildPartitionedAccelerationStructureInfoNV` | Build 配置信息 |
| `VkPartitionedAccelerationStructureInstancesInputNV` | Instance 输入（write / update 操作） |
| `VkPartitionedAccelerationStructureWriteInstanceDataNV` | 写入 instance 数据 |
| `VkPartitionedAccelerationStructureUpdateInstanceDataNV` | 更新已有 instance |
| `VkPartitionedAccelerationStructureWritePartitionTranslationDataNV` | Partition 级 transform |

### 新增 Descriptor Type

`VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV` — 用于 shader binding

### 特殊常量

`VK_PARTITIONED_ACCELERATION_STRUCTURE_PARTITION_INDEX_GLOBAL_NV` — 不属于任何 partition 的全局 instance

### Feature / Properties

- `VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV` — 启用 PTLAS
- `VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV` — 查询限制（max partitions, max instances per partition 等）

---

## 3. 实现范围

### 3.1 RHI 层

在 `mi/rhi/` 中新增 PTLAS 支持：

**新增类型**：
- `RHIPartitionedTLAS` — PTLAS 资源对象（类似现有 `RHIAccelerationStructure`，但类型不同）
- `RHIPTLASBuildInfo` — Build 配置
- `RHIPTLASPartitionDesc` — 单个 partition 的描述（instance 列表、partition index）
- `RHIPTLASInstanceData` — Instance 数据（兼容 PTLAS 格式）

**新增 RHI 接口**：
- `RHI::CreatePartitionedTLAS(max_partitions, max_instances_per_partition)` → `TRef<RHIPartitionedTLAS>`
- `RHI::GetPTLASBuildSizes(build_info)` → size info
- `RHICommandQueue::BuildPartitionedTLAS(build_info, scratch_buffer)` — 构建/更新

**Vulkan 实现**（`mi/rhi/vk/`）：
- `vk_ptlas.cpp` — PTLAS 的 Vulkan 实现
- 在 VulkanDevice 初始化时查询 PTLAS feature/properties
- 在 `FindVulkan.cmake` 中确认扩展版本支持

**参考**：
- NVIDIA sample: https://github.com/nvpro-samples/vk_partitioned_tlas
- Vulkan spec: `VK_NV_partitioned_acceleration_structure`

### 3.2 RDG 层

在 `mi/rdg/` 中集成 PTLAS：

**新增 RDG 资源类型**：
- `RDGPartitionedTLAS` — 图内管理的 PTLAS 资源句柄

**新增 RDG Pass 支持**：
- PTLAS build 可以作为 RDG pass 的一部分执行
- RDG 需要追踪 PTLAS 的读写依赖（barrier 管理）
- PTLAS 的 scratch buffer 可以作为 transient RDG buffer

**与现有 RDG AS 管理的关系**：
- 现有 `RDGRayTracingRegistry` 可能需要扩展以支持 PTLAS 类型的注册
- 或新建独立的 PTLAS registry

### 3.3 Renderer 层

在 `mi/renderer/` 中提供 GigaVoxel 可用的 helper：

**PTLAS Instance Manager**：
- 管理 partition 到 chunk 的映射
- 跟踪哪些 partition 需要 update（dirty partition tracking）
- 提供 per-frame 的 PTLAS update 逻辑：
  1. 收集 dirty partitions
  2. 准备 instance data（add/update/remove）
  3. 提交 PTLAS build command

**Partition 策略**（GigaVoxel 场景）：
- 按空间区域分 partition（如 16×16 chunk grid = 一个 partition）
- 或按 LOD 级别分 partition（VC partition、TFC partition、LFC partitions）
- 或混合策略
- 需要在实现时根据 PTLAS properties 中的限制来确定最优策略

**Helper API 草案**：
- `GigaVoxelTLASManager::AddChunk(chunk_id, lod, blas_ref, transform)` → partition_index
- `GigaVoxelTLASManager::RemoveChunk(chunk_id)`
- `GigaVoxelTLASManager::UpdateChunkTransform(chunk_id, new_transform)`
- `GigaVoxelTLASManager::MarkPartitionDirty(partition_index)`
- `GigaVoxelTLASManager::BuildPTLAS(command_queue)` — 执行 per-frame update

---

## 4. 验收标准

### 功能验证
1. RHI 层单元测试：创建 PTLAS，写入 instances，build，ray query 验证 hit 正确性
2. RDG 集成测试：PTLAS build 在 render graph 中正确执行，barrier 正确
3. Renderer helper 测试：通过 helper API 管理 instances，per-frame update 正确

### 性能验证
4. 10K instances 场景下，单 partition update 开销 < 0.5ms
5. 对比传统 TLAS update vs PTLAS partial update 的性能差异

### 兼容性
6. 不支持 PTLAS 扩展时，fallback 到传统 TLAS（graceful degradation）
7. 与现有 RHI TLAS API 共存，不影响已有 renderer pass

---

## 5. 子任务分解

| # | 子任务 | 层级 | 依赖 |
|---|--------|------|------|
| 1 | 研究 PTLAS extension 细节，确认 API 语义和限制 | - | 无 |
| 2 | RHI: 新增 PTLAS 类型定义和接口声明 | RHI | #1 |
| 3 | RHI: Vulkan PTLAS 实现（vk_ptlas.cpp） | RHI | #2 |
| 4 | RHI: 单元测试（创建、build、ray query） | RHI | #3 |
| 5 | RDG: PTLAS 资源类型和 pass 集成 | RDG | #3 |
| 6 | RDG: barrier / 依赖追踪验证 | RDG | #5 |
| 7 | Renderer: PTLAS Instance Manager helper | Renderer | #5 |
| 8 | Renderer: GigaVoxel partition 策略实现 | Renderer | #7 |
| 9 | 集成测试：传统 TLAS fallback 路径 | All | #7 |
| 10 | 性能 benchmark：PTLAS vs TLAS | All | #8 |

---

## 6. 风险与注意事项

- **NVIDIA Only**: PTLAS 是 NVIDIA 专有扩展。项目已经限定 NVIDIA RTX only，所以这不是问题。但需要确认驱动版本支持（需 Vulkan 1.4+ 和足够新的驱动）。
- **扩展成熟度**: Revision 1 (2025-01-09)，尚未 ratified。API 可能在未来有变动。
- **与 Cluster AS 的关系**: NVIDIA 同时发布了 `VK_NV_cluster_acceleration_structure`，未来 GigaVoxel 的 mesh 可能受益于 cluster AS。但当前 task 不涉及 cluster AS。
- **Descriptor Type**: PTLAS 使用新的 descriptor type (`VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV`)，需要确认与现有 RDG root signature / parameter table 的兼容性。

---

## 7. 参考资料

- Vulkan Extension Spec: `VK_NV_partitioned_acceleration_structure`
- NVIDIA Sample: https://github.com/nvpro-samples/vk_partitioned_tlas
- NVIDIA Blog: https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples/
- NVIDIA RTX Kit: https://developer.nvidia.com/rtx-kit
