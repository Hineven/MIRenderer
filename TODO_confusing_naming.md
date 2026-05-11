1. RHIPipelineResourceType vs RHIParamType — 两组枚举承担不同职责但命名接近
// rhi_types.h — 给 Vulkan descriptor set layout 用的分类
enum class RHIPipelineResourceType { kUniformBuffer, kStorageBuffer, kUAV, kSRV, kSampler, ... };

// rhi_param.h — 给 C++ param struct 声明用的分类  
enum class RHIParamType { kStorageBuffer, kUniformBuffer, kUAVTexture, kSRVTexture, kSampler, ... };
顺序不同: RHIPipelineResourceType 以 UniformBuffer 开头，RHIParamType 以 StorageBuffer 开头
值不同: UAV 在 Pipeline 侧是 kUAV，在 Param 侧是 kUAVTexture
ConvertParamResourceIndexToResourceSlot<RHIParamType::kUniformBuffer>(i) 的模板参数是 RHIParamType，但内部索引的数组 cpp_resource_index_to_slot_ 也在用 (uint32_t)type 做下标，容易让人以为两个 enum 值的数值排列一致
建议: 在 RemapResourceIndexToRHIResourceSlots 处加注释说明 RHIParamType 和 RHIPipelineResourceType 的索引关系，或统一为一种枚举。

2. RemapResourceIndexToRHIResourceSlots / ConvertParamResourceIndexToResourceSlot — "slot"语义在两个重映射间漂移
RemapResourceIndexToRHIResourceSlots: 名字暗示"重映射到RHI slot"，实际干的只是 cpp_resource_index_to_slot_[type][i] = (uint32_t)i（填回自己），只做 "资源是否被pipeline使用"的打表，并在注释里写 cpp_param_index == root_sig_sequential_index
ConvertParamResourceIndexToResourceSlot: 模板函数，调用者传入 RHIParamType 模板参数，返回的 "resource slot" 实际对应的是 root signature 中的 sequential index
建议: RemapResourceIndexToRHIResourceSlots → BuildCppParamUsedByPipelineFlags 或 MarkCppParamAccessibleInPipeline。ConvertParamResourceIndexToResourceSlot → GetRootSignatureSlotForCppParam。

3. 两个 VulkanPipelineBindingRemappings 实例用途不同但共享同一类型
VulkanRootSignature::remappings_: 映射 (type, param_struct_index) → binding，用于 descriptor set 写入
Pipeline CompileRHI 中的 compile_remappings: 映射 (type, pipeline_slot) → binding，用于 SPIR-V 重写
二者都是 VulkanPipelineBindingRemappings，但 key 语义完全不同（param_struct_index vs pipeline内部slot）。

建议:

Root signature 侧的改名为 VulkanDescriptorBindingMap 或 RHISignatureBindingTable
Pipeline 侧的保持 VulkanPipelineBindingRemappings（因为它确实做 pipeline-specific 的重映射）
两者如果不需要共享代码，可以拆分类型
4. BuildRemappingsFromRootSignature 函数名迷惑
函数名暗示"从 root signature 构建 remappings"，但实际是：

遍历 pipeline 的 shader 资源
通过 name CRC 在 root signature 的 type names 中查找匹配
产出 pipeline-internal-slot → Vulkan binding 的映射
建议: → BuildPipelineToRootSignatureBindingRemap 或 RelocateShaderBindingsToRootSignature

5. populate_all 参数名不够表达意图
populate_all = true 的真正语义是 "使用 param struct 声明顺序（= root signature sequential index）作为 slot"，false 是"使用 pipeline 内部 slot 顺序"。名字 populate_all 暗示的是"是否填充全部条目"，而实际上关系到 slot 索引体系的选择。

建议: → use_root_signature_slots / use_pipeline_slots，或直接改为 enum SlotSource { kParamStructOrder, kPipelineOrder }。