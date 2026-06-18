# Task: PTLAS (Partitioned TLAS) Support

**Status**: Planned (设计已定稿，待实现)
**Priority**: P0 (GigaVoxel 基础设施)
**Depends on**: 无
**Estimated Effort**: Large

---

## 1. 背景与动机

### 问题

GigaVoxel 在 64km 视距下可能有数千~上万活跃 chunk instances。传统 TLAS 在 instance 数量变化或个别 instance 更新时，需要 rebuild/update 整个 TLAS 数据结构，开销随 instance 数量线性增长。

每帧可能发生：
- 数百个 chunk 加载完成（add instances）
- 数十个 chunk 离开视距（remove instances）
- 数百个 chunk LOD 转换（update transform / BLAS ref）

传统 `vkCmdBuildAccelerationStructuresKHR` 的 update 模式虽然支持 partial update，但性能和灵活性有限，不适合万级 instance 的高频局部更新场景。

### 解决方案

使用 NVIDIA 的 `VK_NV_partitioned_acceleration_structure` 扩展（PTLAS）。
PTLAS 将 TLAS 分成多个 **partition**，每个 partition 可以独立 update/rebuild，不触发整个 TLAS 的重建。这天然适合 GigaVoxel 的场景：
- 按空间区域（如 chunk grid tile）或按 LOD 级别分组为不同 partition
- 只有发生变化的 partition 需要 rebuild
- 新增/移除 chunk 只影响对应 partition

### 分区规则（已定）

- **VC 阶段**：每个区块一个分区（不使用 GLOBAL 分区）。约 289 个区块 → 289 个分区。
- **TFC/LFC（未来）**：更粗的粒度（区域分组）以减少分区数量。
- 分区 ID 是 **PTLAS 全局的** — 必须通过 PartitionAllocator 在所有渲染对象中唯一。
- 约束：没有两个渲染对象可以共享非全局分区 ID。
- `VK_PARTITIONED_ACCELERATION_STRUCTURE_PARTITION_INDEX_GLOBAL_NV`（=~0U）是特殊值（每个 instance 充当独立分区，不占用 partitionCount）。

---

## 2. Vulkan 扩展关键事实（已从头文件确认）

### 2.1 PTLAS 的本质：仍是 instance 层结构

PTLAS **保留了 instance 层**。`VkPartitionedAccelerationStructureWriteInstanceDataNV` 就是 instance：

```c
typedef struct VkPartitionedAccelerationStructureWriteInstanceDataNV {
    VkTransformMatrixKHR                              transform;           // 3x4，变换在 instance 层
    float                                             explicitAABB[6];     // 世界空间 AABB（需 flag 启用）
    uint32_t                                          instanceID;          // = KHR 的 instanceCustomIndex（24位语义）
    uint32_t                                          instanceMask;        // 8 位可见性 mask
    uint32_t                                          instanceContributionToHitGroupIndex; // SBT 偏移
    VkPartitionedAccelerationStructureInstanceFlagsNV instanceFlags;
    uint32_t                                          instanceIndex;       // PTLAS 内部的线性 instance 索引
    uint32_t                                          partitionIndex;      // 所属分区 ID（核心字段）
    VkDeviceAddress                                   accelerationStructure; // BLAS 设备地址
} ...;
```

**结论**：渲染对象 transform = BLAS transform 的假设保持成立。PTLAS 对 shader 而言功能等价于 TLAS。

### 2.2 Op 类型与对应数据结构

```c
typedef enum VkPartitionedAccelerationStructureOpTypeNV {
    VK_..._OP_TYPE_WRITE_INSTANCE_NV              = 0,  // 写入/重写 instance（增删改）
    VK_..._OP_TYPE_UPDATE_INSTANCE_NV             = 1,  // 只更新 BLAS ref + SBT offset（不动 transform）
    VK_..._OP_TYPE_WRITE_PARTITION_TRANSLATION_NV = 2,  // 写分区级平移（partitionTranslation）
} ...;
```

| Op | 数据结构 | 关键字段 | 用途 |
|----|---------|---------|------|
| WRITE_INSTANCE | `WriteInstanceDataNV` | transform/AABB/ID/mask/SBT/flags/instanceIndex/partitionIndex/blasAddr | 添加/替换 instance |
| UPDATE_INSTANCE | `UpdateInstanceDataNV` | instanceIndex/SBT/blasAddr（**无 transform**） | 仅更新 BLAS ref + SBT |
| WRITE_PARTITION_TRANSLATION | `WritePartitionTranslationDataNV` | partitionIndex/translation[3] | 分区级平移（可选特性） |

**重要约束**：UPDATE_INSTANCE 只能改 BLAS ref 和 SBT offset，**不能改 transform**。改 transform 必须用 WRITE_INSTANCE 重写该 instance。VC 阶段若相机移动导致 chunk transform 变化，必须走 WRITE_INSTANCE 重写整个分区。

### 2.3 Build 接口（间接命令驱动）

即使是"直接式" `vkCmdBuildPartitionedAccelerationStructuresNV`，也是通过 indirect command 数组驱动：

```c
typedef struct VkBuildPartitionedAccelerationStructureInfoNV {
    VkStructureType                                    sType;
    void*                                              pNext;
    VkPartitionedAccelerationStructureInstancesInputNV input;       // 容量描述
    VkDeviceAddress                                    srcAccelerationStructureData;  // update 源
    VkDeviceAddress                                    dstAccelerationStructureData;  // build 目标
    VkDeviceAddress                                    scratchData;
    VkDeviceAddress                                    srcInfos;        // BuildPartitionedAccelerationStructureIndirectCommandNV[]
    VkDeviceAddress                                    srcInfosCount;   // 指向 uint32_t（命令数）
} ...;

typedef struct VkBuildPartitionedAccelerationStructureIndirectCommandNV {
    VkPartitionedAccelerationStructureOpTypeNV opType;   // op 类型
    uint32_t                                   argCount; // 元素数
    VkStridedDeviceAddressNV                   argData;  // strided device address
} ...;
```

多 op 批量提交：一个 `vkCmdBuildPartitionedAccelerationStructuresNV` 调用可以携带多个 indirect command（每个 = 一种 op + 它的数据）。

### 2.4 容量描述：InstancesInputNV

```c
typedef struct VkPartitionedAccelerationStructureInstancesInputNV {
    VkStructureType                      sType;
    void*                                pNext;
    VkBuildAccelerationStructureFlagsKHR flags;
    uint32_t                             instanceCount;                    // 总 instance 数
    uint32_t                             maxInstancePerPartitionCount;     // 每分区最大 instance
    uint32_t                             partitionCount;                   // 分区数（不含 GLOBAL）
    uint32_t                             maxInstanceInGlobalPartitionCount; // GLOBAL 分区最大 instance
} ...;
```

容量在创建时固定，既是 build size 查询输入（`vkGetPartitionedAccelerationStructuresBuildSizesNV`），也是 build 时的 input 字段。

### 2.5 PTLAS 的创建/销毁——没有 vkCreate 函数

**关键**：`vkCreatePartitionedAccelerationStructureNV` 不存在。PTLAS 不是 Vulkan object：
- PTLAS 数据存储由用户分配的 **buffer** 承载（`dstAccelerationStructureData`）
- build 时由 driver 填充该 buffer
- PTLAS 只有 device address + size，没有 Vk 句柄
- 销毁 = 释放 buffer

这与现有 `RHIAccelerationStructure`（KHR，有 `VkAccelerationStructureKHR` 句柄 + `vkCreateAccelerationStructureKHR`）**完全不同**。

### 2.6 Shader 绑定：新的 descriptor type

PTLAS 用 `VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV` 绑定：
```c
typedef struct VkWriteDescriptorSetPartitionedAccelerationStructureNV {
    VkStructureType      sType;
    void*                pNext;
    uint32_t             accelerationStructureCount;
    const VkDeviceAddress* pAccelerationStructures;   // device ADDRESS 数组，非句柄
} ...;
```
**与 KHR TLAS 本质区别**：KHR 用 `VkAccelerationStructureKHR*`（句柄），PTLAS 用 `VkDeviceAddress*`（地址）。

### 2.7 Vulkan-Hpp 支持（已确认）

项目 Vulkan-Headers（`external/VulkanMemoryAllocator-Hpp/Vulkan-Headers`）完整支持本扩展：
- `vk::DescriptorType::ePartitionedAccelerationStructureNV` ✓（vulkan_enums.hpp:3619）
- `vk::WriteDescriptorSetPartitionedAccelerationStructureNV` ✓（vulkan_structs.hpp:143504）
- 所有 struct/enum/函数 wrapper 齐全

### 2.8 物理设备属性

```c
typedef struct VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV {
    uint32_t maxPartitionCount;   // 唯一属性：最大分区数（NVIDIA 通常数千）
} ...;
```

---

## 3. 封装设计（最终定稿）

### 3.0 核心设计决策

**RDG param 的 `AccelerationStructure` 同时接受普通 TLAS 和 PTLAS。** 具体机制：
- **新增** RDG param 宏类型字符串 `PartitionedAccelerationStructure`，映射到**新的** `RHIParamType::kPartitionedAccelerationStructure`
- C++ 侧写 `SHADER_RESOURCE_PARAMETER(PartitionedAccelerationStructure, PTLAS)`
- HLSL 仍声明 `RaytracingAccelerationStructure PTLAS;`（DXC 不区分）
- **SPIRV-Cross 反射**：PTLAS 和 TLAS 都落进同一个 `acceleration_structures` 桶（已确认 spirv_cross.hpp:96 只有一个桶，无法区分）
- **RDG 配对阶段分流**：在 `CheckShaderReflection` 时，按 C++ 参数声明的类型（`kPartitionedAccelerationStructure`），从 SPIRV-Cross 合并桶里把 PTLAS 挑出来，分流到独立子桶
- **root signature** 给 PTLAS 分配独立的 NV descriptor type binding 区段
- **运行时 descriptor write** 走独立的 NV 分支

**解决 layout binding type 矛盾**：root signature 的 `types[]` 表（`vk_root_signature.cpp:29`）在 `kAccelerationStructure` 之后新增一行 PTLAS，用 `vk::DescriptorType::ePartitionedAccelerationStructureNV`，分配独立 binding 区段。因为 RDG 配对已经按 C++ 声明把 PTLAS 拆到独立槽位，layout 创建时能正确知道哪个 binding 是 PTLAS。

### 3.1 RHI 层

**新增资源类**（独立于 `RHIAccelerationStructure`，因为生命周期模型完全不同）：

```cpp
// mi/rhi/include/rhi/rhi_ptlas.h
class RHIPartitionedTLAS : public RHIResource {
public:
    virtual ~RHIPartitionedTLAS() override;
    // 容量（创建时固定）
    virtual uint32_t GetMaxPartitionCount() const = 0;
    virtual uint32_t GetMaxInstancesPerPartition() const = 0;
    virtual uint32_t GetMaxInstancesInGlobalPartition() const = 0;
    virtual uint32_t GetMaxInstanceCount() const = 0;
    // build sizes（vkGetPartitionedAccelerationStructuresBuildSizesNV）
    virtual RHIPartitionedTLASBuildSizes GetBuildSizes(
        const RHIPartitionedTLASInstancesInput& input) const = 0;
    // 分配 backing buffer（size 来自 GetBuildSizes）
    virtual bool Allocate(size_t as_size) = 0;
    // build 后的 device address（给 descriptor write 用）
    virtual uint64_t GetDeviceAddress() const = 0;
    // 用于 barrier（覆盖 backing buffer）
    virtual RHIResource* GetBackingBuffer() = 0;  // 或 vk::Buffer GetBuffer()
    virtual size_t GetSize() const = 0;
};
```

**数据结构**（对应 NV struct）：

```cpp
// PTLAS instance（WRITE_INSTANCE op）
struct RHIPartitionedTLASInstance {
    float        transform[12];                 // 3x4 row-major
    float        explicit_aabb[6];              // 世界空间 AABB
    uint32_t     instance_id;                   // 24 位 customIndex（shell|chunk 编码）
    uint8_t      instance_mask;
    uint32_t     instance_contribution_to_hit_group_index;  // SBT 偏移
    RHIPTLASInstanceFlags instance_flags;       // 新 flag enum
    uint32_t     instance_index;                // PTLAS 内部线性索引
    uint32_t     partition_index;               // 所属分区（或 GLOBAL）
    uint64_t     acceleration_structure;        // BLAS 设备地址
};
// UPDATE_INSTANCE op
struct RHIPartitionedTLASInstanceUpdate {
    uint32_t instance_index;
    uint32_t instance_contribution_to_hit_group_index;
    uint64_t acceleration_structure;
};
// build 时的多 op 批量提交
struct RHIPartitionedTLASBuildOp {
    RHIPTLASOpType op_type;       // WRITE_INSTANCE / UPDATE_INSTANCE / WRITE_PARTITION_TRANSLATION
    uint32_t       arg_count;
    const void*    arg_data;      // host 端数据数组
    uint64_t       arg_stride;    // sizeof(对应 struct)
};
// 容量描述
struct RHIPartitionedTLASInstancesInput {
    RHIAccelerationStructureBuildFlags flags;
    uint32_t instance_count;
    uint32_t max_instance_per_partition_count;
    uint32_t partition_count;
    uint32_t max_instance_in_global_partition_count;
};
```

**arg_data 上传职责**：renderer 负责（方案 A）。renderer 把 instance 数据上传到 device buffer，只把 buffer span 传给 RHI。RHI 负责生成 indirect command 数组 + 上传。与现有 TLAS instance buffer 上传路径一致（mi_renderer.cpp:359 `builder.CreateBuffer + upload_context`）。

**Build 命令**：

```cpp
// rhi_cmd.h
class RHICommandBuildPartitionedTLAS : public TRHICommand<...> {
public:
    RHICommandBuildPartitionedTLAS(
        RHIPartitionedTLAS* dst_ptlas,
        RHIPartitionedTLAS* src_ptlas,          // update 模式用，build 时 nullptr
        RHIBufferSpan       scratch_buffer,
        RHIBufferSpan       indirect_commands_buffer,   // BuildPartitionedAccelerationStructureIndirectCommandNV[]
        RHIBufferSpan       indirect_commands_count,    // uint32_t
        RHIPartitionedTLASInstancesInput input);
    void Execute(RHICommandQueueBase&) override;
};
// RHICommandQueueGraphics 便捷 API
void BuildPartitionedTLAS(...);
```

**Vulkan 后端**（`mi/rhi/vk/vk_ptlas.cpp`）：
- `VulkanPartitionedTLAS`：buffer 分配（VMA，usage = `eShaderDeviceAddress`）、`GetDeviceAddress`、`GetBuildSizes`（调 `getPartitionedAccelerationStructuresBuildSizesNV`）、`Allocate`
- 后端 build 执行：生成 indirect command 数组 → 上传 → 调 `buildPartitionedAccelerationStructuresNV`

**扩展启用**（`vk_rhi.cpp`）：
- 扩展列表（L231 区域）加 `VK_NV_PARTITIONED_ACCELERATION_STRUCTURE_EXTENSION_NAME`
- StructureChain（L322 区域）加 `vk::PhysicalDevicePartitionedAccelerationStructureFeaturesNV`（`.setPartitionedAccelerationStructure(VK_TRUE)`）
- properties 查询加 `vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV`，存 `maxPartitionCount`
- **函数指针无需手写**：dispatcher（`vk_dispatcher.cpp:9`）启用扩展后自动加载

### 3.2 RDG 层（reflection 分流）

**核心：名字约定 + 配对阶段分流。** 不改 SPIRV-Cross reflection 本身，只在 RDG 配对时区分。

改动点：
1. `rhi_param.h`：`RHIParamType` 加 `kPartitionedAccelerationStructure`；`RHITypeNameStringToParamType` 加 `"PartitionedAccelerationStructure"` → `kPartitionedAccelerationStructure`；`ToString` 加分支
2. `rhi_types.h`：`RHIPipelineResourceType` 加 `kPartitionedAccelerationStructure`（在 `kAccelerationStructure` 后、`kMax` 前）
3. `rdg_param.h`：新增 `TRDGShaderParamPlaceHolderType<"PartitionedAccelerationStructure">`，C++ 成员类型 `RHIPartitionedTLAS*`
4. `rdg_param.cpp`：finalize 时把 `kPartitionedAccelerationStructure` 收集到独立 span
5. `rdg_root_signature_cache.cpp`：`FillRootSignatureDescFromParamInfo` 加 PTLAS 的 `num_resources` + name CRC 填充
6. `rdg_shader.cpp::CheckShaderReflection`：**关键分流点**——遍历 `shader->GetAccelerationStructureDesc()`（SPIRV-Cross 合并桶）配对时，按 C++ 参数的实际 `RHIParamType` 区分，把 PTLAS 挑到独立子列表
7. `rdg_shader.cpp::RemapResourceIndexToRootSigResourceIndex`：加 PTLAS 段
8. `rdg_cmd.cpp`：从参数结构取 PTLAS 指针，填到 `RHIBindPipelineParametersDesc::partitioned_acceleration_structures`

**HLSL 侧**：仍声明 `RaytracingAccelerationStructure PTLAS;`。DXC 编译出的 SPIR-V 用 `OpTypeAccelerationStructureKHR`，SPIRV-Cross 归到 `acceleration_structures` 桶。区分完全靠 C++ 侧 `SHADER_RESOURCE_PARAMETER(PartitionedAccelerationStructure, ...)` 的声明。

### 3.3 RHI 层 descriptor write（VK 后端）

改动点：
1. `vk_conversion.h::GetVulkanDescriptorType`：加 `kPartitionedAccelerationStructure → ePartitionedAccelerationStructureNV`
2. `vk_root_signature.cpp:29`：`types[]` 加一行 PTLAS（NV descriptor type），分配独立 binding 区段
3. `vk_cmd_exec.cpp::BuildDescriptorWritesForTable`：加 PTLAS 分支——pNext 挂 `WriteDescriptorSetPartitionedAccelerationStructureNV`，`pAccelerationStructures = &VkDeviceAddress`（来自 `GetDeviceAddress()`）
4. `vk_cmd_exec_common.cpp::CreateFrameTemporaryDescriptorPool`：pool_sizes 加 `ePartitionedAccelerationStructureNV` + 上限常量（`vk_constants.h` 加 `kMaxNumPartitionedAccelerationStructureDescriptorsPerFrame`）
5. `vk_pipeline.cpp::RelocateShaderResourceBindings` + `BuildRemappingsFromRootSignature`：加 PTLAS 段（SPIR-V 字节码 binding 重定位）

### 3.4 Renderer 层

renderer 层已为此铺好路：
- `RenderableBLASInstance{blas, transform, instance_custom_index, instance_mask, sbt_offset, partition_index, explicit_aabb}`（`mi_renderable.h`）—— 字段一一映射到 PTLAS instance
- `PartitionAllocator`（`mi_partition_allocator.h`）—— 驱动部分重建（ConsumeDirty）
- `GigaVoxel::GetPartitionedBLASInstances()` —— 返回带 partition_index 的 instance span
- `GigaVoxelInstance::Update` 已直连 RHI queue（绕过 RDG，`mi_giga_voxel.cpp:410`）

改动点：
1. `mi_renderer.cpp` TLAS gathering：加 PTLAS 分支——按 partition 分组生成 WRITE_INSTANCE ops
2. GigaVoxel `explicit_aabb` 填充（`RebuildCachedInstances` 当前是 TODO 空）
3. 两阶段构建：首帧 build（src=nullptr），后续帧 update（src=旧 PTLAS）

---

## 4. 验收标准

### 功能验证
1. RHI 层单元测试：创建 PTLAS，写入 instances，build，ray query 验证 hit 正确性
2. RDG 集成：PTLAS 通过 `PartitionedAccelerationStructure` param 绑定到 ray tracing shader
3. GigaVoxel 通过 PTLAS 参与 RT，per-chunk BLAS 按 partition 分组

### 兼容性
4. 不影响现有 KHR TLAS 路径（StaticMesh 等）
5. PTLAS 扩展不可用时，fallback 到传统 TLAS（graceful degradation）

---

## 5. 分阶段实施

| # | 子任务 | 层级 | 依赖 |
|---|--------|------|------|
| 1 | 启用 NV 扩展 + feature + properties | RHI | 无 |
| 2 | RHIPartitionedTLAS 类 + 数据结构 + Vulkan 后端实现 | RHI | #1 |
| 3 | BuildPartitionedTLAS 命令 + 后端执行 | RHI | #2 |
| 4 | RHI 层单元测试（创建、build、ray query） | RHI | #3 |
| 5 | RDG param 宏 + reflection 分流 + root signature | RDG | #2 |
| 6 | VK descriptor write 分支 + pool + 重定位 | RHI/RDG | #5 |
| 7 | renderer PTLAS gathering + build 路径 | Renderer | #5,#6 |
| 8 | GigaVoxel explicit_aabb 填充 + 集成 | Renderer | #7 |
| 9 | 集成测试：GigaVoxel 通过 PTLAS RT | All | #8 |

---

## 6. 风险与注意事项

- **NVIDIA Only**：PTLAS 是 NVIDIA 专有扩展。项目已限定 NVIDIA RTX only。
- **扩展成熟度**：Revision 1 (2025-01-09)，尚未 ratified。API 可能在未来有变动。
- **UPDATE_INSTANCE 限制**：只能改 BLAS ref + SBT，不能改 transform。VC 阶段相机移动若改 chunk transform 必须 WRITE_INSTANCE 重写分区。
- **HLSL 声明**：HLSL 没有内置 PTLAS 类型，仍用 `RaytracingAccelerationStructure`。区分完全靠 C++ 侧 param 宏声明。这要求开发者遵守约定：`PartitionedAccelerationStructure` 宏对应 PTLAS 资源，普通 `AccelerationStructure` 对应 KHR TLAS。误用会导致 root signature binding type 不匹配。

---

## 7. 参考资料

- Vulkan Extension Spec: `VK_NV_partitioned_acceleration_structure`
- NVIDIA Sample: https://github.com/nvpro-samples/vk_partitioned_tlas
- NVIDIA Blog: https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples/
- 项目 RT 设计文档: `applications/macromc/planning/gigavoxel/GIGAVOXEL_RT_DESIGN.md`
