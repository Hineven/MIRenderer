/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_pipeline.h"

#include <iostream>
#include <spirv-tools/optimizer.hpp>

#include "vk_shader.h"
#include "rhi/rhi_texture.h"

#include "rhi_device_shared.h"
#include "vk_bindless.h"
#include "vk_root_signature.h"
#include "core/rounding.h"
#include "vk_conversion.h"
#include "vk_texture.h"

MI_NAMESPACE_BEGIN

struct VkShaderModuleKeeper {
    vk::ShaderModule module {};
    FORCEINLINE VkShaderModuleKeeper(vk::ShaderModule module) : module(module) {}
    VkShaderModuleKeeper(const VkShaderModuleKeeper &) = delete;
    FORCEINLINE VkShaderModuleKeeper(VkShaderModuleKeeper && t): module(t.module) {
        t.module = nullptr;
    }
    FORCEINLINE ~VkShaderModuleKeeper() {
        if (module) {
            auto device = GetVulkanRHI()->GetDevice();
            device.destroy(module);
        }
    }
};

static void RelocateShaderResourceBindings (
    RHIPipeline * pipeline, vk::Device device, RHIShader * shader, VulkanPipelineBindingRemappings & remappings,
    std::vector<vk::PipelineShaderStageCreateInfo> & shader_stages, std::vector<VkShaderModuleKeeper> & shader_module_keepers
) {
    if(!shader) return ;
    // Duplicate bytecode for compilation
    auto ir = shader->DuplicateShaderIRByteCode();
    auto Relocate = [&] (ShaderReflection::IRBindingDecorationLocation ir_location, uint32_t set, uint32_t binding) {
        ((uint32_t*)ir.data())[ir_location.binding_offset] = binding;
        ((uint32_t*)ir.data())[ir_location.set_offset] = set;
    };
    auto RelocateResourcesInIR = [&] (RHIPipelineResourceType type, const auto & shader_resources) {
        // Relocate binding numbers in the shader IR
        for (int i = 0; i < (int)shader_resources.size(); i++) {
            auto shader_resource_desc = shader_resources[i];
            // Shader resources should always be present in the pipeline resources
            auto pipeline_res_slot = pipeline->ReflectResourceSlot(shader_resources[i].name_crc);
            auto remap = remappings.GetDestination(
                type, pipeline_res_slot.slot_index
            );
            // Modify the bytecode to actually use the remapped binding in the shader
            Relocate(shader_resource_desc.locations, remap.set, remap.binding);
        }
    };

    // Relocate resources in the shader IR to pipeline binding
    RelocateResourcesInIR(RHIPipelineResourceType::kUniformBuffer, shader->GetUniformBufferDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kStorageBuffer, shader->GetStorageBufferDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kUAV, shader->GetUAVDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kSRV, shader->GetSRVDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kSampler, shader->GetSamplerDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kAccelerationStructure, shader->GetAccelerationStructureDesc());

    // Relocate bindless resource arrays in the shader IR
    if (shader->HasBindlessResources()) {
        auto desc = shader->GetBindlessArrayDescs();
        if (!desc.storage_buffer.name.empty()) {
            Relocate(desc.storage_buffer.locations, 1, (uint32_t)RHIBindlessResourceType::kReadOnlyStorageBuffer);
        }
        if (!desc.srv.name.empty()) {
            Relocate(desc.srv.locations, 1, (uint32_t)RHIBindlessResourceType::kSRV);
        }
        if (!desc.volume_srv.name.empty()) {
            Relocate(desc.volume_srv.locations, 1, 2);
        }
        if (!desc.acceleration_structure.name.empty()) {
            Relocate(desc.acceleration_structure.locations, 1, (uint32_t)RHIBindlessResourceType::kAccelerationStructure);
        }
        if (!desc.volume_srv.name.empty()) {
            Relocate(desc.volume_srv.locations, 1, (uint32_t)RHIBindlessResourceType::kVolumeSRV);
        }
    }

    std::vector<uint32_t> optimized_ir;
    // Because we removed '-spirv-reflect' from dxc default parameters, now we do not need to strip reflection info.
    if (false) {
        // Strip the extensions declared to support shader reflection produced by dxc if present
        // This is a workaround for the issue that the reflection extensions is not supported
        // by NVIDIA drivers. Anyway they are just annotations and won't affect real shader behavior.
        spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
        auto pass_token = spvtools::CreateStripNonSemanticInfoPass();
        optimizer.RegisterPass(std::move(pass_token));
        if(!optimizer.Run((uint32_t*)ir.data(), ir.size() / 4, &optimized_ir)) {
            MI_LOG(MIInfraLogType::kWarning, "Failed to strip reflection info from SPIRV IR.");
            return ;
        }
    } else {
        optimized_ir.resize(ir.size() / 4);
        memcpy(optimized_ir.data(), ir.data(), ir.size());
    }
    // Another way is to simply mute pCode-08742.

    auto create_info = vk::ShaderModuleCreateInfo()
            .setCodeSize(optimized_ir.size() * sizeof(uint32_t))
            .setPCode(optimized_ir.data());
    auto vk_shader = device.createShaderModule(create_info);
    shader_stages.push_back(vk::PipelineShaderStageCreateInfo()
                                    .setStage(GetVulkanShaderStage(shader->GetFrequency()))
                                    .setModule(vk_shader)
                                    .setPName(shader->GetEntryName().c_str()));
    shader_module_keepers.emplace_back(vk_shader);
};

template<typename T>
static auto GetResourceArraySize(T& obj) {
    if constexpr (requires { obj.array_size; }) { return obj.array_size; }
    else { return 0u; }
}

static bool BuildRemappingsFromRootSignature(
    RHIPipelineRootSignature * root, VulkanPipelineBindingRemappings & remappings,
    const auto & uniform_buffers, const auto & storage_buffers,
    const auto & uavs, const auto & srvs, const auto & samplers,
    const auto & acceleration_structures)
{
    auto BuildForType = [&](const auto & pipeline_resources, RHIPipelineResourceType type) -> bool {
        auto & type_names = root->GetTypeNames((uint32_t)type);
        for (int i = 0; i < (int)pipeline_resources.size(); i++) {
            uint32_t param_idx = UINT32_MAX;
            for (uint32_t j = 0; j < type_names.count; j++) {
                if (pipeline_resources[i].name_crc == type_names.name_crcs[j]) {
                    param_idx = j;
                    break;
                }
            }
            if (param_idx != UINT32_MAX) {
                remappings.AddRemapping(type, i, 0, root->GetBinding(type, param_idx));
            } else {
                MI_LOG(MIInfraLogType::kError,
                    "Pipeline resource '{}' (type {}) not found in root signature. "
                    "Shader accesses a resource that does not exist in the pipeline layout.",
                    pipeline_resources[i].name, (uint32_t)type);
                return false;
            }
        }
        return true;
    };
    if (!BuildForType(uniform_buffers, RHIPipelineResourceType::kUniformBuffer)) return false;
    if (!BuildForType(storage_buffers, RHIPipelineResourceType::kStorageBuffer)) return false;
    if (!BuildForType(uavs, RHIPipelineResourceType::kUAV)) return false;
    if (!BuildForType(srvs, RHIPipelineResourceType::kSRV)) return false;
    if (!BuildForType(samplers, RHIPipelineResourceType::kSampler)) return false;
    if (!BuildForType(acceleration_structures, RHIPipelineResourceType::kAccelerationStructure)) return false;
    return true;
}

bool VulkanGraphicsPipeline::CompileRHI(const RHIGraphicsPipelineDesc & pipeline_info, RHIPipelineRootSignature * root_signature) {
    auto device = GetVulkanRHI()->GetDevice();

    VulkanPipelineBindingRemappings compile_remappings;

    mi_assert(root_signature, "Root signature must not be null");

    auto * vk_root = static_cast<VulkanRootSignature*>(root_signature);
    root_signature_ = vk_root;
    vk_pipeline_layout_ = vk_root->GetPipelineLayout();
    vk_private_descriptor_set_layout_ = vk_root->GetDescriptorSetLayout();
    push_constant_roundup_size_ = RoundUp(root_signature->GetPushConstantSize(), 128);
    if (!BuildRemappingsFromRootSignature(root_signature, compile_remappings,
        uniform_buffers_, storage_buffers_, uavs_, srvs_, samplers_, acceleration_structures_)) {
        return false;
    }

    // Specify creation configuration
    vk::GraphicsPipelineCreateInfo pipeline_info_vk {};

    std::vector<vk::PipelineShaderStageCreateInfo> shader_stages;
    std::vector<VkShaderModuleKeeper> shader_module_keepers;
    // Shader stages
    {
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.vertex_shader, compile_remappings, shader_stages, shader_module_keepers);
        // RelocateShaderResourceBindings(this, device, pipeline_info.stages.tess_control_shader, compile_remappings, shader_stages, shader_module_keepers);
        // RelocateShaderResourceBindings(this, device, pipeline_info.stages.tess_evaluation_shader, compile_remappings, shader_stages, shader_module_keepers);
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.geometry_shader, compile_remappings, shader_stages, shader_module_keepers);
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.fragment_shader, compile_remappings, shader_stages, shader_module_keepers);
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.task_shader, compile_remappings, shader_stages, shader_module_keepers);
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.mesh_shader, compile_remappings, shader_stages, shader_module_keepers);
        pipeline_info_vk.setStages(shader_stages);
    }

    // Vertex input
    vk::PipelineVertexInputStateCreateInfo vertex_input_vk {};
    std::vector<vk::VertexInputBindingDescription> vertex_buffers;
    std::vector<vk::VertexInputAttributeDescription> vertex_attributes;
    {
        for(auto & buffer_binding : pipeline_info.vertex_input.vertex_buffers) {
            vertex_buffers.push_back(vk::VertexInputBindingDescription()
                                              .setBinding(buffer_binding.binding)
                                              .setStride(buffer_binding.stride)
                                              .setInputRate(vk::VertexInputRate::eVertex));
        }
        for(auto & attribute : pipeline_info.vertex_input.vertex_attributes) {
            vertex_attributes.push_back(vk::VertexInputAttributeDescription()
                                                 .setLocation(attribute.location)
                                                 .setBinding(attribute.src_binding)
                                                 .setFormat(GetVulkanVertexAttributeFormat(attribute.format))
                                                 .setOffset(attribute.offset));
        }
        // Validate the vertex input using reflection
        if(pipeline_info.stages.vertex_shader) {
            const auto & inputs = pipeline_info.stages.vertex_shader->GetVertexInputDesc();
            if(inputs.size() < vertex_attributes.size()) {
                MI_LOG(MIInfraLogType::kWarning, "Pipeline {}: There're fewer vertex inputs "
                                                 "used than specified in the pipeline configuration.", GetName());
            }
            for(int i = 0; i < (int)vertex_attributes.size(); ++i) {
                bool found = false;
                for(int j = 0; j < (int)inputs.size(); j++) {
                    if(inputs[j].location == vertex_attributes[i].location) {
                        found = true;
                        if(inputs[j].format != GetRHIVertexAttributeFormat(vertex_attributes[i].format)) {
                            MI_LOG(MIInfraLogType::kWarning, "Pipeline {}: Vertex input format mismatch for vertex shader.",
                                GetName());
                            return false;
                        }
                        break;
                    }
                }
                if(!found) {
                    // Do nothing, as we allow the shader to have fewer inputs than the pipeline.
                }
            }
        }
        vertex_input_vk.setVertexBindingDescriptions(vertex_buffers);
        vertex_input_vk.setVertexAttributeDescriptions(vertex_attributes);
        pipeline_info_vk.setPVertexInputState(&vertex_input_vk);
    }

    // Input assembly
    vk::PipelineInputAssemblyStateCreateInfo input_assembly_vk {};
    {
        input_assembly_vk.setTopology(GetVulkanPrimitiveTopology(pipeline_info.topology));
        pipeline_info_vk.setPInputAssemblyState(&input_assembly_vk);
    }

    vk::PipelineTessellationStateCreateInfo tessellation_vk {};
    {
        // Tess is not supported by RHI for now
        //        pipeline_info_vk.setPTessellationState(&tessellation_vk);
    }

    // The viewport and rasterization states are partially dynamic
    vk::PipelineRasterizationStateCreateInfo rast_vk {};
    {
        rast_vk.rasterizerDiscardEnable = pipeline_info.rasterization_discard;
        pipeline_info_vk.setPRasterizationState(&rast_vk);
    }

    vk::PipelineMultisampleStateCreateInfo multisample_vk {};
    {
        // Multisample is not supported by RHI for now
        multisample_vk.setRasterizationSamples(vk::SampleCountFlagBits::e1);
        pipeline_info_vk.setPMultisampleState(&multisample_vk);
    }

    vk::PipelineDepthStencilStateCreateInfo depth_stencil_vk {};
    {
        depth_stencil_vk.setDepthTestEnable(pipeline_info.depth_stencil.depth_test_enable);
        depth_stencil_vk.setDepthWriteEnable(pipeline_info.depth_stencil.depth_write_enable);
        depth_stencil_vk.setDepthCompareOp(GetVulkanCompareOp(pipeline_info.depth_stencil.depth_compare_op));
        depth_stencil_vk.setDepthBoundsTestEnable(false);
        depth_stencil_vk.setStencilTestEnable(false);
        // Ignore stencil operations
        // Ignore depth bounds
        pipeline_info_vk.setPDepthStencilState(&depth_stencil_vk);
    }

    std::vector<vk::PipelineColorBlendAttachmentState> blend_attachments;
    vk::PipelineColorBlendStateCreateInfo color_blend_vk {};
    {
        color_blend_vk.setLogicOpEnable(false);
        color_blend_vk.setLogicOp(vk::LogicOp::eCopy);
        for(auto & attachment : pipeline_info.color_attachments) {
            auto & blending = attachment.blending;
            bool blend_enable = blending.blend_enable;
            if (blend_enable && IsUIntPixelFormat(attachment.format)) {
                MI_LOG(MIInfraLogType::kWarning,
                       "Pipeline {}: blending is not supported for integer color attachment format {}, disabling blending.",
                       GetName(), GetPixelFormatName(attachment.format));
                blend_enable = false;
            }
            blend_attachments.push_back(vk::PipelineColorBlendAttachmentState()
                                         .setBlendEnable(blend_enable)
                                         .setSrcColorBlendFactor(GetVulkanBlendFactor(blending.src_color_blend_factor))
                                         .setDstColorBlendFactor(GetVulkanBlendFactor(blending.dst_color_blend_factor))
                                         .setColorBlendOp(GetVulkanBlendOp(blending.color_blend_op))
                                         .setSrcAlphaBlendFactor(GetVulkanBlendFactor(blending.src_alpha_blend_factor))
                                         .setDstAlphaBlendFactor(GetVulkanBlendFactor(blending.dst_alpha_blend_factor))
                                         .setAlphaBlendOp(GetVulkanBlendOp(blending.alpha_blend_op))
                                         .setColorWriteMask(GetVulkanColorWriteMask(attachment.format)));
        }
        color_blend_vk.setAttachments(blend_attachments);
        pipeline_info_vk.setPColorBlendState(&color_blend_vk);
    }

    vk::PipelineDynamicStateCreateInfo dynamic_state_vk {};
    std::vector<vk::DynamicState> dynamic_states {
        vk::DynamicState::eViewportWithCount,
        // vk::DynamicState::eViewport,
        vk::DynamicState::eScissorWithCount,
        // vk::DynamicState::eScissor,
        vk::DynamicState::eDepthClampEnableEXT,
//            vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::ePolygonModeEXT,
        vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,
        vk::DynamicState::eDepthBiasEnable,
        vk::DynamicState::eDepthBias,
        vk::DynamicState::eLineWidth
};
    {
        dynamic_state_vk.setDynamicStateCount((int)dynamic_states.size());
        dynamic_state_vk.setPDynamicStates(dynamic_states.data());
        pipeline_info_vk.setPDynamicState(&dynamic_state_vk);
    }

    pipeline_info_vk.setLayout(vk_pipeline_layout_);

    bool has_depth_stencil = pipeline_info.depth_stencil_attachment.format != PixelFormatType::kUnknown;
    // Dynamic rendering support
    auto color_attachment_formats = std::vector<vk::Format>(pipeline_info.color_attachments.size());

    for(int i = 0; i < pipeline_info.color_attachments.size(); ++i) {
        color_attachment_formats[i] = GetVulkanPixelFormat(
                pipeline_info.color_attachments[i].format
        );
    }
    auto rdn_info = vk::PipelineRenderingCreateInfo {
                {}, color_attachment_formats,
                has_depth_stencil ? vk::Format::eD32Sfloat : vk::Format::eUndefined,
                {}
    };
    pipeline_info_vk.setPNext(&rdn_info);

    {
        auto guard = std::lock_guard(GetVulkanRHI()->GetPipelineCacheMutex());
        auto result = device.createGraphicsPipeline(GetVulkanRHI()->GetPipelineCache(), pipeline_info_vk);
        if(result.result != vk::Result::eSuccess) {
            MI_LOG(MIInfraLogType::kWarning, "Failed to create graphics pipeline: %s. Error code: %s", GetName(), vk::to_string(result.result));
            vk_pipeline_layout_ = nullptr;
            vk_private_descriptor_set_layout_ = nullptr;
            root_signature_ = nullptr;
            return false;
        }
        vk_pipeline_ = result.value;
    }

    SetName(GetName());

    return true;
}

void VulkanGraphicsPipeline::ResetRHI() {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroy(vk_pipeline_);
    vk_pipeline_ = nullptr;
    vk_pipeline_layout_ = nullptr;
    vk_private_descriptor_set_layout_ = nullptr;
    root_signature_ = nullptr;

    push_constant_roundup_size_ = 0;
}

void VulkanGraphicsPipeline::SetName(const std::string& name) {
    RHIGraphicsPipeline::SetName(name);
#if MI_ENABLE_RHI_OBJECT_NAMING
    if (vk_pipeline_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::ePipeline,
            (uint64_t)(VkPipeline)vk_pipeline_,
            GetName()
        });
    }
    if (vk_pipeline_layout_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::ePipelineLayout,
            (uint64_t)(VkPipelineLayout)vk_pipeline_layout_,
            (GetName() + std::string("_layout")).c_str()
        });
    }
    if (vk_private_descriptor_set_layout_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::eDescriptorSetLayout,
            (uint64_t)(VkDescriptorSetLayout)vk_private_descriptor_set_layout_,
            (GetName() + std::string("_private_desc_set_layout")).c_str()
        });
    }
#endif
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline() {
    VulkanGraphicsPipeline::ResetRHI();
}

void *VulkanGraphicsPipeline::GetAPIHandle() const {
    return (void*)vk_pipeline_;
}


// Called from parent's constructor
bool VulkanComputePipeline::CompileRHI (RHIShader *shader, RHIPipelineRootSignature * root_signature) {
    auto device = GetVulkanRHI()->GetDevice();
    auto compute_shader = static_cast<VulkanShader *>(shader);

    VulkanPipelineBindingRemappings compile_remappings;

    mi_assert(root_signature, "Root signature must not be null");

    auto * vk_root = static_cast<VulkanRootSignature*>(root_signature);
    root_signature_ = vk_root;
    vk_pipeline_layout_ = vk_root->GetPipelineLayout();
    vk_private_descriptor_set_layout_ = vk_root->GetDescriptorSetLayout();
    push_constant_roundup_size_ = RoundUp(root_signature->GetPushConstantSize(), 128);
    if (!BuildRemappingsFromRootSignature(root_signature, compile_remappings,
        uniform_buffers_, storage_buffers_, uavs_, srvs_, samplers_, acceleration_structures_)) {
        return false;
    }

    std::vector<vk::PipelineShaderStageCreateInfo> shader_stages;
    std::vector<VkShaderModuleKeeper> shader_module_keepers;
    RelocateShaderResourceBindings(this, device, compute_shader, compile_remappings, shader_stages, shader_module_keepers);

    {
        auto guard = std::lock_guard(GetVulkanRHI()->GetPipelineCacheMutex());
        auto result = device.createComputePipeline(
                GetVulkanRHI()->GetPipelineCache(),
                vk::ComputePipelineCreateInfo()
                        .setLayout(vk_pipeline_layout_)
                        .setStage(shader_stages[0])
        );

        if(result.result != vk::Result::eSuccess) {
            MI_LOG(MIInfraLogType::kWarning, "Failed to create compute pipeline: %s.", GetName());
            vk_pipeline_layout_ = nullptr;
            vk_private_descriptor_set_layout_ = nullptr;
            root_signature_ = nullptr;
            return false;
        }
        vk_pipeline_ = result.value;
    }

    SetName(GetName());

    return true;
}

void VulkanComputePipeline::ResetRHI() {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroy(vk_pipeline_);
    vk_pipeline_layout_ = nullptr;
    vk_pipeline_ = nullptr;
    vk_private_descriptor_set_layout_ = nullptr;
    root_signature_ = nullptr;

    push_constant_roundup_size_ = 0;
}

void VulkanComputePipeline::SetName(const std::string& name) {
    RHIComputePipeline::SetName(name);
#if MI_ENABLE_RHI_OBJECT_NAMING
    if (vk_pipeline_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::ePipeline,
            (uint64_t)(VkPipeline)vk_pipeline_,
            GetName()
        });
    }
    if (vk_pipeline_layout_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::ePipelineLayout,
            (uint64_t)(VkPipelineLayout)vk_pipeline_layout_,
            (GetName() + std::string("_layout")).c_str()
        });
    }
    if (vk_private_descriptor_set_layout_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::eDescriptorSetLayout,
            (uint64_t)(VkDescriptorSetLayout)vk_private_descriptor_set_layout_,
            (GetName() + std::string("_private_desc_set_layout")).c_str()
        });
    }
#endif
}

VulkanComputePipeline::~VulkanComputePipeline() {
    VulkanComputePipeline::ResetRHI();
}

void *VulkanComputePipeline::GetAPIHandle() const {
    return (void*)vk_pipeline_;
}

// Ray tracing pipeline implementation
uint32_t VulkanRayTracingPipeline::GetShaderGroupHandleSize() const {
    return shader_group_handle_size_;
}

bool VulkanRayTracingPipeline::GetShaderGroupHandles(uint32_t first_group, uint32_t group_count, void* data) const {
    if (!vk_pipeline_ || !data) {
        return false;
    }
    size_t data_size = group_count * GetShaderGroupHandleSize();
    auto result = GetVulkanRHI()->GetDevice().getRayTracingShaderGroupHandlesKHR(
        vk_pipeline_, first_group, group_count, data_size, data);
    return result == vk::Result::eSuccess;
}

uint32_t VulkanRayTracingPipeline::GetShaderGroupHandleAlignment() const {
    return shader_group_handle_alignment_;
}

uint32_t VulkanRayTracingPipeline::GetShaderGroupBaseAlignment() const {
    return shader_group_base_alignment_;
}

// SBT stride methods implementation
uint32_t VulkanRayTracingPipeline::GetRaygenSBTStride() const {
    return raygen_sbt_stride_;
}

uint32_t VulkanRayTracingPipeline::GetMissSBTStride() const {
    return miss_sbt_stride_;
}

uint32_t VulkanRayTracingPipeline::GetHitSBTStride() const {
    return hit_sbt_stride_;
}

uint32_t VulkanRayTracingPipeline::GetCallableSBTStride() const {
    return callable_sbt_stride_;
}

void VulkanRayTracingPipeline::SetName(const std::string& name) {
    RHIPipeline::SetName(name);
#ifdef MI_DEBUG
    if (vk_pipeline_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::ePipeline,
            (uint64_t)(VkPipeline)vk_pipeline_,
            GetName()
        });
    }
    if (vk_pipeline_layout_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::ePipelineLayout,
            (uint64_t)(VkPipelineLayout)vk_pipeline_layout_,
            (GetName() + std::string("_layout")).c_str()
        });
    }
    if (vk_private_descriptor_set_layout_) {
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
            vk::ObjectType::eDescriptorSetLayout,
            (uint64_t)(VkDescriptorSetLayout)vk_private_descriptor_set_layout_,
            (GetName() + std::string("_private_desc_set_layout")).c_str()
        });
    }
#endif
}

VulkanRayTracingPipeline::~VulkanRayTracingPipeline() {
    VulkanRayTracingPipeline::ResetRHI();
}

void* VulkanRayTracingPipeline::GetAPIHandle() const {
    return (void*)vk_pipeline_;
}

bool VulkanRayTracingPipeline::CompileRHI(const RHIRayTracingPipelineDesc& desc, RHIPipelineRootSignature * root_signature) {
    auto device = GetVulkanRHI()->GetDevice();

    VulkanPipelineBindingRemappings compile_remappings;
    // Get ray tracing properties from RHI device properties
    auto props = GetVulkanRHI()->GetDeviceProperties();
    shader_group_handle_size_ = props.shader_group_handle_size;
    shader_group_handle_alignment_ = props.shader_group_handle_alignment;
    shader_group_base_alignment_ = props.shader_group_base_alignment;

    mi_assert(root_signature, "Root signature must not be null");

    // Build remappings from the root signature descriptor set layout
    auto * vk_root = static_cast<VulkanRootSignature*>(root_signature);
    root_signature_ = vk_root;
    vk_pipeline_layout_ = vk_root->GetPipelineLayout();
    vk_private_descriptor_set_layout_ = vk_root->GetDescriptorSetLayout();
    push_constant_roundup_size_ = RoundUp(root_signature->GetPushConstantSize(), 128);
    if (!BuildRemappingsFromRootSignature(root_signature, compile_remappings,
        uniform_buffers_, storage_buffers_, uavs_, srvs_, samplers_, acceleration_structures_)) {
        return false;
    }

    // Create shader stages with resource binding relocation
    std::vector<vk::PipelineShaderStageCreateInfo> shader_stages;
    std::vector<VkShaderModuleKeeper> shader_module_keepers;

    for (auto* shader : desc.shaders) {
        if (shader) {
            RelocateShaderResourceBindings(this, device, shader, compile_remappings, shader_stages, shader_module_keepers);
        }
    }

    // Convert shader groups
    std::vector<vk::RayTracingShaderGroupCreateInfoKHR> shader_groups;
    shader_groups.reserve(desc.shader_groups.size());

    for (const auto& group : desc.shader_groups) {
        vk::RayTracingShaderGroupCreateInfoKHR vk_group{};

        switch (group.type) {
            case RHIRayTracingShaderGroupType::kRayGeneration:
            case RHIRayTracingShaderGroupType::kMiss:
            case RHIRayTracingShaderGroupType::kCallable:
                vk_group.type = vk::RayTracingShaderGroupTypeKHR::eGeneral;
                vk_group.generalShader = group.general_shader_index;
                vk_group.closestHitShader = VK_SHADER_UNUSED_KHR;
                vk_group.anyHitShader = VK_SHADER_UNUSED_KHR;
                vk_group.intersectionShader = VK_SHADER_UNUSED_KHR;
                break;

            case RHIRayTracingShaderGroupType::kTrianglesHitGroup:
                vk_group.type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup;
                vk_group.generalShader = VK_SHADER_UNUSED_KHR;
                vk_group.closestHitShader = (group.closest_hit_shader_index != UINT32_MAX) ?
                    group.closest_hit_shader_index : VK_SHADER_UNUSED_KHR;
                vk_group.anyHitShader = (group.any_hit_shader_index != UINT32_MAX) ?
                    group.any_hit_shader_index : VK_SHADER_UNUSED_KHR;
                vk_group.intersectionShader = VK_SHADER_UNUSED_KHR;
                break;

            case RHIRayTracingShaderGroupType::kProceduralHitGroup:
                vk_group.type = vk::RayTracingShaderGroupTypeKHR::eProceduralHitGroup;
                vk_group.generalShader = VK_SHADER_UNUSED_KHR;
                vk_group.closestHitShader = (group.closest_hit_shader_index != UINT32_MAX) ?
                    group.closest_hit_shader_index : VK_SHADER_UNUSED_KHR;
                vk_group.anyHitShader = (group.any_hit_shader_index != UINT32_MAX) ?
                    group.any_hit_shader_index : VK_SHADER_UNUSED_KHR;
                vk_group.intersectionShader = (group.intersection_shader_index != UINT32_MAX) ?
                    group.intersection_shader_index : VK_SHADER_UNUSED_KHR;
                break;
        }

        shader_groups.push_back(vk_group);
    }

    // Create ray tracing pipeline
    vk::RayTracingPipelineCreateInfoKHR pipeline_info{};
    pipeline_info.stageCount = static_cast<uint32_t>(shader_stages.size());
    pipeline_info.pStages = shader_stages.data();
    pipeline_info.groupCount = static_cast<uint32_t>(shader_groups.size());
    pipeline_info.pGroups = shader_groups.data();
    pipeline_info.maxPipelineRayRecursionDepth = desc.max_recursion_depth;
    pipeline_info.layout = vk_pipeline_layout_;

    {
        auto guard = std::lock_guard(GetVulkanRHI()->GetPipelineCacheMutex());
        auto result = device.createRayTracingPipelineKHR(nullptr, nullptr, pipeline_info);
        if (result.result != vk::Result::eSuccess) {
            MI_LOG(MIInfraLogType::kWarning, "Failed to create Vulkan ray tracing pipeline: %s", vk::to_string(result.result));
            vk_pipeline_layout_ = nullptr;
            vk_private_descriptor_set_layout_ = nullptr;
            root_signature_ = nullptr;
            return false;
        }
        vk_pipeline_ = result.value;
    }

    // Calculate SBT strides based on shader group handle size and alignment
    // SBT stride = shader group handle size + user data size (aligned)
    // For now, we assume no user data, so stride = aligned handle size
    auto aligned_handle_size = RoundUp(shader_group_handle_size_, shader_group_base_alignment_);

    // For raygen, typically only one entry, so stride equals handle size
    raygen_sbt_stride_ = aligned_handle_size;

    // For miss, hit, and callable shaders, use the base alignment
    miss_sbt_stride_ = aligned_handle_size;
    hit_sbt_stride_ = aligned_handle_size;
    callable_sbt_stride_ = aligned_handle_size;

    // Store shader group count in base class
    shader_group_count_ = static_cast<uint32_t>(desc.shader_groups.size());
    max_recursion_depth_ = desc.max_recursion_depth;

    SetName(GetName());
    return true;
}

void VulkanRayTracingPipeline::ResetRHI() {
    auto device = GetVulkanRHI()->GetDevice();

    if (vk_pipeline_) {
        device.destroyPipeline(vk_pipeline_);
        vk_pipeline_ = nullptr;
    }
    vk_pipeline_layout_ = nullptr;
    vk_private_descriptor_set_layout_ = nullptr;

    root_signature_ = nullptr;

    shader_group_handle_size_ = 0;
    shader_group_handle_alignment_ = 0;
    shader_group_base_alignment_ = 0;
    push_constant_roundup_size_ = 0;
}

MI_NAMESPACE_END
