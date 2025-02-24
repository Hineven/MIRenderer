/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_pipeline.h"
#include "vk_shader.h"
#include "rhi/rhi_texture.h"

#include "rhi_device_shared.h"
#include "vk_bindless.h"
#include "core/rounding.h"
#include "vk_conversion.h"
#include "vk_texture.h"

MI_NAMESPACE_BEGIN

bool VulkanGraphicsPipeline::CompileRHI(const RHIGraphicsPipelineDesc & pipeline_info) {
    auto device = GetVulkanRHI()->GetDevice();

    // Gather pipeline layout
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
                    for (auto &res: desc) {
                        bindfull_bindings.emplace_back()
                                .setBinding(current_binding_index)
                                .setDescriptorType(type)
                                .setDescriptorCount(1)
                                .setStageFlags(GetVulkanShaderStageFlags(res.frequency_bits));
                        current_binding_index++;
                    }
                }
                for (int i = 0; i < (int) desc.size(); ++i) {
                    remappings_.AddRemapping(rhi_type, i, set_index, current_binding_index + i);
                }
                current_binding_index += (int)desc.size();
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
    // Shader stages
    {
        auto PushShaderStage = [&] (RHIShader * shader) {
            if(!shader) return ;
            auto vk_shader = static_cast<VulkanShader *>(shader);
            shader_stages.push_back(vk::PipelineShaderStageCreateInfo()
                                            .setStage(GetVulkanShaderStage(shader->GetFrequency()))
                                            .setModule(vk_shader->GetShaderModule())
                                            .setPName(shader->GetEntryName().c_str()));
        };
        PushShaderStage(pipeline_info.stages.vertex_shader);
        PushShaderStage(pipeline_info.stages.fragment_shader);
        PushShaderStage(pipeline_info.stages.geometry_shader);
        PushShaderStage(pipeline_info.stages.task_shader);
        PushShaderStage(pipeline_info.stages.mesh_shader);
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
            vk::DynamicState::eScissorWithCount,
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

//    std::vector<vk::AttachmentDescription> attachments_vk;
//    for(auto & attachment : pipeline_info.color_attachments) {
//        vk::ImageLayout initial_layout = vk::ImageLayout::eColorAttachmentOptimal;
//        // We can not use eUndefined as initial layout as the user may assume that the texture
//        // data is preserved before and after the render pass if it's untouched.
////        if(attachment.load_op != RHILoadOpType::kLoad) {
////            initial_layout = vk::ImageLayout::eUndefined;
////        }
//        auto desc = vk::AttachmentDescription()
//                .setFormat(GetVulkanPixelFormat(attachment.format))
//                .setSamples(vk::SampleCountFlagBits::e1)
//                .setInitialLayout(initial_layout)
//                .setFinalLayout(vk::ImageLayout::eColorAttachmentOptimal);
//        attachments_vk.push_back(desc);
//    }
//    auto subpass_color_attachments_vk = std::vector<vk::AttachmentReference>(attachments_vk.size());
//    for(int i = 0; i < attachments_vk.size(); ++i) {
//        subpass_color_attachments_vk[i].setAttachment(i);
//        subpass_color_attachments_vk[i].setLayout(vk::ImageLayout::eColorAttachmentOptimal);
//    }
    bool has_depth_stencil = pipeline_info.depth_stencil_attachment.format != PixelFormatType::kUnknown;
//    if(has_depth_stencil) {
//        auto desc = vk::AttachmentDescription()
//                .setFormat(GetVulkanPixelFormat(pipeline_info.depth_stencil_attachment.format))
//                .setSamples(vk::SampleCountFlagBits::e1)
//                .setLoadOp(GetVulkanLoadOp(pipeline_info.depth_stencil_attachment.load_op))
//                .setStoreOp(GetVulkanStoreOp(pipeline_info.depth_stencil_attachment.store_op))
//                .setStencilLoadOp(GetVulkanLoadOp(pipeline_info.depth_stencil_attachment.load_op))
//                .setStencilStoreOp(GetVulkanStoreOp(pipeline_info.depth_stencil_attachment.store_op))
//                .setInitialLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
//                .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
//        // Append depth stencil attachment as the last attachment
//        attachments_vk.push_back(desc);
//    }
//    auto subpass_depth_stencil_vk = vk::AttachmentReference()
//            .setAttachment((uint32_t)attachments_vk.size() - 1)
//            .setLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
//
//    auto subpass_vk = vk::SubpassDescription()
//            .setColorAttachments(subpass_color_attachments_vk);
//    if(has_depth_stencil) subpass_vk.setPDepthStencilAttachment(&subpass_depth_stencil_vk);
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
//
//    // We do not use multiple subpasses as we are targeting at desktop level IBR devices.
//    pipeline_info_vk.setSubpass(0);

    auto result = device.createGraphicsPipeline(GetVulkanRHI()->GetPipelineCache(), pipeline_info_vk);
    if(result.result != vk::Result::eSuccess) {
        MI_LOG(MIInfraLogType::kWarning, "Failed to create graphics pipeline: %s.", GetName());
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
    auto compute_stage = vk::PipelineShaderStageCreateInfo()
            .setStage(vk::ShaderStageFlagBits::eCompute)
            .setModule(compute_shader->GetShaderModule())
            .setPName("Main");

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
    push_constant_roundup_size_ = RoundUp(command_constant_[0].size, 128);
    push_constant_range.setSize(push_constant_roundup_size_);
    push_constant_range.setStageFlags(vk::ShaderStageFlagBits::eCompute);
    // Create pipeline layout
    vk_pipeline_layout_ = device.createPipelineLayout(
            vk::PipelineLayoutCreateInfo()
                    .setSetLayoutCount((int)descriptor_set_layouts.size())
                    .setPSetLayouts(descriptor_set_layouts.data())
                    .setPushConstantRangeCount(1)
                    .setPPushConstantRanges(&push_constant_range)
    );

    // Create pipeline
    auto result = device.createComputePipeline(
            GetVulkanRHI()->GetPipelineCache(),
            vk::ComputePipelineCreateInfo()
                    .setLayout(vk_pipeline_layout_)
                    .setStage(compute_stage)
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