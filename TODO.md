# MIRenderer Refactoring TODO

## Phase 1: Pipeline Layout / Root Signature Sharing + Descriptor Set复用

### 问题
- 每个Shader独占一套 `vk::DescriptorSetLayout` + `vk::PipelineLayout`，编译时创建
- 具有相同参数shape的shader（如DiffuseIndirectLighting阶段的~25个Compute Shader）各自创建重复的layout
- dispatch时每次分配新descriptor set并写入，造成频繁GPU上下文切换

### 目标
- 引入 Pipeline Layout Cache，根据RDG Shader Params的identity自动推断共享
- 相同signature的shader共享 `vk::DescriptorSetLayout` 和 `vk::PipelineLayout`
- 共享同一signature的连续dispatch之间复用descriptor set，仅更新变化部分
- 完全向后兼容——现有shader不需要任何修改即可受益

---

### 设计决策（已确认）

- **Signature key**: `RDGShaderParamStructAndSizeInfo*`（指针identity）+ `has_bindless` + `push_constant_size`。
  无需名称注册表。运行时缓存，指针identity在同一运行期内绝对稳定。
- **Push constant**: 包含在signature中。目前没有大量使用push constant，后续再优化。
- **Set安排**: 延续现有（set 0 = signature-specific bindful, set 1 = bindless）。
- **Descriptor set复用的param hash**: 包含resource handle + offset，不含size。
- **SPIR-V binding重写**: 共享signature的所有shader的binding被统一重写为cache分配的固定值。

---

### Step 1: RDG层 — Pipeline Signature

**`PipelineLayoutSignature`** 定义在 `rdg_param.h` 中：

```cpp
struct PipelineLayoutSignature {
    const RDGShaderParamStructAndSizeInfo * param_struct_info; // 指针identity作为核心key
    bool has_bindless;
    uint32_t push_constant_size;

    size_t Hash() const;
    bool operator==(const PipelineLayoutSignature&) const;
};
```

**实现**：
- 在 `RDGShaderParamStructAndSizeInfo` 中添加 `PipelineLayoutSignature signature_` 字段
- 在 `zzFinalizeTopLevelParamsStructInfo` 末尾计算signature（此时已知所有参数信息和push constant）
- Push constant size从参数表中的dispatch command或uniform buffer推断，或先设为0（当前未使用push constant）

**涉及文件**：
- `mi/rdg/include/rdg/rdg_param.h` — 新增 `PipelineLayoutSignature` 定义，`RDGShaderParamStructAndSizeInfo` 添加signature字段
- `mi/rdg/rdg_param.cpp` — signature计算

---

### Step 2: RHI层 — Pipeline Layout Cache

**`PipelineLayoutCache`** 定义在 `mi/rhi/vk/` 中，作为 `VulkanRHI` 的成员：

```cpp
struct CachedPipelineLayout {
    vk::DescriptorSetLayout descriptor_set_layout;
    vk::PipelineLayout pipeline_layout;
    VulkanPipelineBindingRemappings remappings;
    uint32_t push_constant_roundup_size;
    PipelineLayoutSignature signature;
    // Descriptor set复用相关
    vk::DescriptorSet last_descriptor_set {}; // 最近一次分配的descriptor set
    size_t last_param_hash {};               // 最近一次的param hash
    bool last_descriptor_set_valid {false};
};

class PipelineLayoutCache {
    struct SignatureHash { size_t operator()(const PipelineLayoutSignature&) const; };
    std::unordered_map<PipelineLayoutSignature, CachedPipelineLayout, SignatureHash> cache_;
public:
    CachedPipelineLayout* GetOrCreate(
        const PipelineLayoutSignature& sig,
        const PipelineReflectionData& reflection, // 从pipeline reflection获取binding信息
        vk::Device device
    );
    void Clear(vk::Device device);
};
```

**重构三种 `CompileRHI`**：
1. 提取公共的 "创建bindings → 创建descriptor set layout → 创建pipeline layout → 填充remappings" 为 `PipelineLayoutCache::GetOrCreate` 内的逻辑
2. Graphics/Compute/RayTracing 的 `CompileRHI` 都改为：接收signature → 查cache → 获取cached layout → 用cached layout的remappings做SPIR-V relocation → 用cached layout的pipeline layout创建vk::Pipeline
3. Pipeline析构时不再destroy descriptor set layout和pipeline layout（由cache拥有）
4. Pipeline仍持有 `vk::Pipeline`（这是per-pipeline的，不能共享）

**涉及文件**：
- 新增 `mi/rhi/vk/vk_pipeline_layout_cache.h` + `.cpp` — cache定义
- `mi/rhi/vk/vk_pipeline.h` — pipeline持有 `CachedPipelineLayout*` 而非独立的Vulkan layout对象
- `mi/rhi/vk/vk_pipeline.cpp` — 重构三种 `CompileRHI`
- `mi/rhi/vk/vk_rhi.h` — 添加cache成员

---

### Step 3: Signature传递路径

**传递链**：
```
RDGShaderParamStructAndSizeInfo::signature_
    → RDGShader编译时读取
    → RHIPipeline::Compile(root_signature) 复用已有参数
    → VulkanPipeline::CompileRHI(root_signature)
    → 查PipelineLayoutCache
```

- `RHIPipelineRootSignature` 改为包含 `PipelineLayoutSignature`
- `RHIPipeline::Compile` 和各子类的 `CompileRHI` 已有 `root_signature` 参数，改为实际传入
- `RDGShader` 编译时从 `GetParamStructInfo()->signature_` 读取，构造 `RHIPipelineRootSignature` 传入

**涉及文件**：
- `mi/rhi/include/rhi/rhi_desc.h` — 改造 `RHIPipelineRootSignature` 包含signature
- `mi/rdg/include/rdg/rdg_shader.h` — `RDGShader` 编译时传递signature
- `mi/rdg/rdg_shader.cpp` — 编译逻辑
- `mi/rhi/include/rhi/rhi_pipeline.h` — Compile接口签名调整

---

### Step 4: Descriptor Set复用

**4a. RDG层 — Param Hash计算**

在 `SetupShaderParams` 返回时计算param hash：
```
hash = 0
for each uniform: hash_combine(hash, buffer.handle, offset)
for each storage: hash_combine(hash, buffer.handle, offset)
for each uav:     hash_combine(hash, texture.handle, array_layer, mip_level)
for each srv:     hash_combine(hash, texture.handle, array_layer, mip_level)
for each sampler: hash_combine(hash, sampler.handle)
for each as:      hash_combine(hash, as.handle)
```

返回值扩展为包含hash的结构，或在 `RHIBindPipelineParametersDesc` 中添加hash字段。

**4b. RHI层 — FlushBindPointState优化**

`FlushBindPointState` 中的优化路径：
```
if (layout与上次相同) {
    if (param_hash == cached_layout.last_param_hash) {
        // 完全相同！跳过descriptor write和bind
        仅bind pipeline（如果dirty）
        return;
    }
    if (cached_layout.last_descriptor_set_valid) {
        // layout相同但参数不同：allocate新descriptor set，write，bind
        // 后续优化：可以做per-binding dirty tracking只write变化部分
    }
}
// layout不同：走完整流程
```

**涉及文件**：
- `mi/rdg/rdg_cmd.cpp` — `SetupShaderParams` 添加param hash计算
- `mi/rhi/include/rhi/rhi_desc.h` — `RHIBindPipelineParametersDesc` 添加param hash
- `mi/rhi/vk/vk_cmd_exec.cpp` — `FlushBindPointState` 添加复用逻辑

---

### Step 5: 清理与验证 — ✅ 完成

- Pipeline析构逻辑：不再destroy cached Vulkan对象 ✅
- PipelineLayoutCache在VulkanRHI销毁时统一清理 ✅
- 循环include依赖已修复 (vk_pipeline_layout_cache.h 不再依赖 vk_rhi.h) ✅
- 待用户在CLion中编译验证
- NSight Graphics确认layout共享生效（减少unique descriptor set layout数量）
- DiffuseIndirectLighting阶段dispatch之间descriptor set创建数量显著下降

---

### Follow-up: RHI层 Descriptor Set Cache

当前实现（Step 4）只在每个BindPoint上缓存"上一次"的descriptor set，仅在连续两次相同参数dispatch时跳过allocate+write。更优方案是在RHI层维护一个per-frame descriptor set pool/cache：

```
Key: (CachedPipelineLayout*, param_hash)
Value: vk::DescriptorSet
```

- 同一帧内遇到相同 sig + hash 的dispatch时，直接复用已创建的descriptor set
- 完全省去 `allocateDescriptorSets` + `CompileShaderDescriptorWrites` + `updateDescriptorSets` 调用
- Cache在帧结束（descriptor pool reset）时自动失效
- 对于DiffuseIndirectLighting的~25个共享参数表compute shader，如果它们在同一帧内绑定相同的资源（如CommonViewParams、LightStructure等），可以实现跨pipeline的descriptor set复用

---

## Phase 2: Shader Param Include 功能 (RDG层)

### 问题
- 大量shader parameter在不同shader甚至不同cpp文件中重复定义
- 缺乏"shader param table include"机制

### 目标
- 在 `BEGIN_SHADER_PARAMETERS`...`END_SHADER_PARAMETERS` 内引入 `INCLUDE_SHADER_PARAMETERS(SharedParams)` 指令

### 实现计划（Phase 1完成后细化）

#### 2.1 INCLUDE_SHADER_PARAMETERS 宏设计
- 运行时append：展开为调用被include表的 `InitParamStructInfo` + append其参数到当前params vector
- 处理 cpp_offset 重映射问题

#### 2.2 提取公共参数表
- `CommonViewParams`, `CommonGeometryParams`, `CommonSamplerParams`, `LightStructureParams`

#### 2.3 与Phase 1协同
- Include使更多shader共享参数表 → 相同param struct指针 → 自动共享pipeline layout + descriptor set
