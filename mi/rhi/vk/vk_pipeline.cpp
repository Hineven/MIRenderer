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
    auto RelocateResourcesInIR = [&] (RHIPipelineResourceType type, const auto & shader_resources) {
        // Relocate binding numbers in the shader IR
        for (int i = 0; i < (int)shader_resources.size(); i++) {
            auto shader_resource_desc = shader_resources[i];
            // Shader resources should always be present in the pipeline resources
            auto pipeline_res_slot = pipeline->ReflectResourceSlot(shader_resources[i].name_crc);
            auto remap = remappings.GetDestination(
                type, pipeline_res_slot.slot_index
            );
            printf("[%d][%s]original binding: %u\n",
                (int)shader_stages.size(),
                shader_resource_desc.name.c_str(), ((uint32_t*)ir.data())[shader_resource_desc.locations.binding_offset]);
            std::flush(std::cout);
            // Modify the bytecode to actually use the remapped binding in the shader
            ((uint32_t*)ir.data())[shader_resource_desc.locations.binding_offset] = remap.binding;
            ((uint32_t*)ir.data())[shader_resource_desc.locations.set_offset] = remap.set;
        }
    };
    RelocateResourcesInIR(RHIPipelineResourceType::kUniformBuffer, shader->GetUniformBufferDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kStorageBuffer, shader->GetStorageBufferDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kUAV, shader->GetUAVDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kSRV, shader->GetSRVDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kSampler, shader->GetSamplerDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kImmutableSampler, shader->GetImmutableSamplerDesc());
    RelocateResourcesInIR(RHIPipelineResourceType::kAccelerationStructure, shader->GetAccelerationStructureDesc());

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

bool VulkanGraphicsPipeline::CompileRHI(const RHIGraphicsPipelineDesc & pipeline_info) {
    auto device = GetVulkanRHI()->GetDevice();

    // Gather pipeline layout, align descriptor bindings
    {
        std::vector<vk::DescriptorSetLayout> descriptor_set_layouts;
        // If the pipeline contains bindless resources, take set 0 as bindless set.
        if (HasBindlessResources()) {
            // Use set 0 for bindless resources.
            VulkanBindlessManager & bindless_manager = static_cast<mi::VulkanBindlessManager &>(RHI::Get().GetBindlessManager());
            auto bindless_descriptor_layout = bindless_manager.GetBindlessDescriptorSetLayout();
            descriptor_set_layouts.push_back(bindless_descriptor_layout);
            // No remapping required for bindless resources
        }
        std::vector<vk::DescriptorSetLayoutBinding> bindfull_bindings;
        // Take the next descriptor set for bindfull resources
        {
            int set_index = (int)descriptor_set_layouts.size();
            int current_binding_index = 0;

            auto AddBindings = [&](const auto &desc, vk::DescriptorType type, RHIPipelineResourceType rhi_type) {
                if (!desc.empty()) {
                    int i = 0;
                    for (auto &res: desc) {
                        bindfull_bindings.emplace_back()
                                .setBinding(current_binding_index)
                                .setDescriptorType(type)
                                .setDescriptorCount(1)
                                .setStageFlags(GetVulkanShaderStageFlags(res.frequency_bits));
                        remappings_.AddRemapping(rhi_type, i, set_index, current_binding_index);
                        i ++, current_binding_index ++;
                    }
                }
            };
            AddBindings(uniform_buffers_, vk::DescriptorType::eUniformBuffer, RHIPipelineResourceType::kUniformBuffer);
            AddBindings(storage_buffers_, vk::DescriptorType::eStorageBuffer, RHIPipelineResourceType::kStorageBuffer);
            AddBindings(uavs_, vk::DescriptorType::eStorageImage, RHIPipelineResourceType::kUAV);
            AddBindings(srvs_, vk::DescriptorType::eSampledImage, RHIPipelineResourceType::kSRV);
            AddBindings(samplers_, vk::DescriptorType::eSampler, RHIPipelineResourceType::kSampler);

            if (!immutable_samplers_.empty()) {
                // TODO support immutable samplers
                mi_assert(false, "Immutable samplers are not implemented currently.");
            }

            if (!bindfull_bindings.empty()) {
                auto descriptor_set_layout = device.createDescriptorSetLayout(
                        vk::DescriptorSetLayoutCreateInfo()
                                .setBindingCount((int)bindfull_bindings.size())
                                .setPBindings(bindfull_bindings.data())
                );
                descriptor_set_layouts.push_back(descriptor_set_layout);
                vk_private_descriptor_set_layout_ = descriptor_set_layout;
            } else {
                vk_private_descriptor_set_layout_ = nullptr;
            }
        }
        // Push constant
        vk::PushConstantRange push_constant_range;
        push_constant_range.setOffset(0);
        push_constant_roundup_size_ = RoundUp(command_constant_.size() > 0 ? command_constant_[0].size : 0, 128);
        push_constant_range.setSize(push_constant_roundup_size_);
        // TODO track push constant shader stages
        push_constant_range.setStageFlags(vk::ShaderStageFlagBits::eAll);
        // Create pipeline layout
        vk_pipeline_layout_ = device.createPipelineLayout(
                vk::PipelineLayoutCreateInfo()
                        .setSetLayoutCount((uint32_t)descriptor_set_layouts.size())
                        .setPSetLayouts(descriptor_set_layouts.data())
                        .setPushConstantRangeCount(push_constant_range.size > 0 ? 1 : 0)
                        .setPPushConstantRanges(push_constant_range.size > 0
                            ? (&push_constant_range) : nullptr)
        );
    }

    // Specify creation configuration
    vk::GraphicsPipelineCreateInfo pipeline_info_vk {};

    std::vector<vk::PipelineShaderStageCreateInfo> shader_stages;
    std::vector<VkShaderModuleKeeper> shader_module_keepers;
    // Shader stages
    {
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.vertex_shader, remappings_, shader_stages, shader_module_keepers);
        // RelocateShaderResourceBindings(this, device, pipeline_info.stages.tess_control_shader, remappings_, shader_stages, shader_module_keepers);
        // RelocateShaderResourceBindings(this, device, pipeline_info.stages.tess_evaluation_shader, remappings_, shader_stages, shader_module_keepers);
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.geometry_shader, remappings_, shader_stages, shader_module_keepers);
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.fragment_shader, remappings_, shader_stages, shader_module_keepers);
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.task_shader, remappings_, shader_stages, shader_module_keepers);
        RelocateShaderResourceBindings(this, device, pipeline_info.stages.mesh_shader, remappings_, shader_stages, shader_module_keepers);
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
            if(inputs.size() != vertex_attributes.size()) {
                MI_LOG(MIInfraLogType::kWarning, "Vertex input count mismatch.");
                return false;
            }
            for(int i = 0; i < (int)vertex_attributes.size(); ++i) {
                bool found = false;
                for(int j = 0; j < (int)inputs.size(); j++) {
                    if(inputs[j].location == vertex_attributes[i].location) {
                        found = true;
                        if(inputs[j].format != GetRHIVertexAttributeFormat(vertex_attributes[i].format)) {
                            MI_LOG(MIInfraLogType::kWarning, "Vertex input format mismatch for vertex shader.");
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
        rast_vk.rasterizerDiscardEnable = false;
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
            blend_attachments.push_back(vk::PipelineColorBlendAttachmentState()
                                         .setBlendEnable(blending.blend_enable)
                                         .setSrcColorBlendFactor(GetVulkanBlendFactor(blending.src_color_blend_factor))
                                         .setDstColorBlendFactor(GetVulkanBlendFactor(blending.dst_color_blend_factor))
                                         .setColorBlendOp(GetVulkanBlendOp(blending.color_blend_op))
                                         .setSrcAlphaBlendFactor(GetVulkanBlendFactor(blending.src_alpha_blend_factor))
                                         .setDstAlphaBlendFactor(GetVulkanBlendFactor(blending.dst_alpha_blend_factor))
                                         .setAlphaBlendOp(GetVulkanBlendOp(blending.alpha_blend_op))
                                         .setColorWriteMask(vk::ColorComponentFlagBits::eA |
                                                            vk::ColorComponentFlagBits::eR |
                                                            vk::ColorComponentFlagBits::eG |
                                                            vk::ColorComponentFlagBits::eB));
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
    {
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
    }

    auto result = device.createGraphicsPipeline(GetVulkanRHI()->GetPipelineCache(), pipeline_info_vk);
    if(result.result != vk::Result::eSuccess) {
        MI_LOG(MIInfraLogType::kWarning, "Failed to create graphics pipeline: %s. Error code: %s", GetName(), vk::to_string(result.result));
        device.destroy(vk_pipeline_layout_);
        device.destroy(vk_render_pass_);
        vk_pipeline_layout_ = nullptr;
        vk_render_pass_ = nullptr;
        return false;
    }
    vk_pipeline_ = result.value;
    return true;
}

void VulkanGraphicsPipeline::ResetRHI() {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroy(vk_pipeline_);
    device.destroy(vk_pipeline_layout_);
    device.destroy(vk_render_pass_);
    device.destroy(vk_private_descriptor_set_layout_);
    vk_pipeline_ = nullptr;
    vk_pipeline_layout_ = nullptr;
    vk_render_pass_ = nullptr;
    vk_private_descriptor_set_layout_ = nullptr;
    remappings_.Reset();
    push_constant_roundup_size_ = 0;
}

void VulkanGraphicsPipeline::OnNameChanged() {
#ifndef NDEBUG
    GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
        vk::ObjectType::ePipeline,
        (uint64_t)(VkPipeline)vk_pipeline_,
        name_.c_str()
    });
#endif
}

VulkanGraphicsPipeline::~VulkanGraphicsPipeline() {
    ResetRHI();
}

// Called from parent's constructor
bool VulkanComputePipeline::CompileRHI (RHIShader *shader) {
    auto device = GetVulkanRHI()->GetDevice();
    auto compute_shader = static_cast<VulkanShader *>(shader);

    // Gather pipeline layout
    std::vector<BindingRemappingInfo> remapping_infos;
    std::vector<vk::DescriptorSetLayout> descriptor_set_layouts;
    // If the pipeline contains bindless resources, take set 0 as bindless set.
    if(HasBindlessResources()) {
        // Use set 0 for bindless resources.
        auto bindless_descriptor_layout = GetVulkanRHI()->GetVulkanBindlessManager()->GetBindlessDescriptorSetLayout();
        descriptor_set_layouts.push_back(bindless_descriptor_layout);
        // No remapping required for bindless resources
    }
    std::vector<vk::DescriptorSetLayoutBinding> bindfull_bindings;
    // Take the next descriptor set for bindfull resources
    {
        int set_index = (int)descriptor_set_layouts.size();
        int current_binding_index = 0;

        auto AddBindings = [&] (const auto & desc, vk::DescriptorType type, RHIPipelineResourceType rhi_type) {
            if(!desc.empty()) bindfull_bindings.emplace_back()
                        .setBinding(current_binding_index)
                        .setDescriptorType(type)
                        .setDescriptorCount((int)desc.size())
                        .setStageFlags(vk::ShaderStageFlagBits::eCompute);
            for(int i = 0; i < (int)desc.size(); ++i) {
                remappings_.AddRemapping(rhi_type, i, set_index, current_binding_index + i);
            }
            current_binding_index += (int)desc.size();
        };
        AddBindings(uniform_buffers_, vk::DescriptorType::eUniformBuffer, RHIPipelineResourceType::kUniformBuffer);
        AddBindings(storage_buffers_, vk::DescriptorType::eStorageBuffer, RHIPipelineResourceType::kStorageBuffer);
        AddBindings(uavs_, vk::DescriptorType::eStorageImage, RHIPipelineResourceType::kUAV);
        AddBindings(srvs_, vk::DescriptorType::eSampledImage, RHIPipelineResourceType::kSRV);
        AddBindings(samplers_, vk::DescriptorType::eSampler, RHIPipelineResourceType::kSampler);

        if(!immutable_samplers_.empty()) {
            // TODO
            mi_assert(false, "Immutable samplers are not implemented currently.");
        }

        if(!bindfull_bindings.empty()) {
            auto descriptor_set_layout = device.createDescriptorSetLayout(
                    vk::DescriptorSetLayoutCreateInfo()
                            .setBindingCount((int)bindfull_bindings.size())
                            .setPBindings(bindfull_bindings.data())
            );
            descriptor_set_layouts.push_back(descriptor_set_layout);
            vk_private_descriptor_set_layout_ = descriptor_set_layout;
        } else {
            vk_private_descriptor_set_layout_ = nullptr;
        }
    }
    // Push constant
    vk::PushConstantRange push_constant_range;
    push_constant_range.setOffset(0);
    push_constant_roundup_size_ = RoundUp(command_constant_.size() > 0 ? command_constant_[0].size : 0, 128);
    push_constant_range.setSize(push_constant_roundup_size_);
    // TODO track push constant shader stages
    push_constant_range.setStageFlags(vk::ShaderStageFlagBits::eAll);
    // Create pipeline layout
    auto info = vk::PipelineLayoutCreateInfo{}
        .setSetLayoutCount((int)descriptor_set_layouts.size())
        .setPSetLayouts(descriptor_set_layouts.data());
    if (push_constant_roundup_size_ > 0) {
        info.setPushConstantRanges(push_constant_range);
    }
    vk_pipeline_layout_ = device.createPipelineLayout(info);

    std::vector<vk::PipelineShaderStageCreateInfo> shader_stages;
    std::vector<VkShaderModuleKeeper> shader_module_keepers;
    // Relocate shader resource bindings in IR, compile vk shader modules
    RelocateShaderResourceBindings(this, device, compute_shader, remappings_, shader_stages, shader_module_keepers);

    // Create pipeline
    auto result = device.createComputePipeline(
            GetVulkanRHI()->GetPipelineCache(),
            vk::ComputePipelineCreateInfo()
                    .setLayout(vk_pipeline_layout_)
                    .setStage(shader_stages[0])
    );

    if(result.result != vk::Result::eSuccess) {
        MI_LOG(MIInfraLogType::kWarning, "Failed to create compute pipeline: %s.", GetName());
        device.destroy(vk_pipeline_layout_);
        return false;
    }
    vk_pipeline_ = result.value;

    return true;
}

void VulkanComputePipeline::ResetRHI() {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroy(vk_pipeline_);
    device.destroy(vk_pipeline_layout_);
    device.destroy(vk_private_descriptor_set_layout_);
    vk_pipeline_layout_ = nullptr;
    vk_pipeline_ = nullptr;
    vk_private_descriptor_set_layout_ = nullptr;
    remappings_.Reset();
    push_constant_roundup_size_ = 0;
}

void VulkanComputePipeline::OnNameChanged() {
#ifndef NDEBUG
    GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT({
        vk::ObjectType::ePipeline,
        (uint64_t)(VkPipeline)vk_pipeline_,
        name_.c_str()
    });
#endif
}

VulkanComputePipeline::~VulkanComputePipeline() {
    ResetRHI();
}

MI_NAMESPACE_END