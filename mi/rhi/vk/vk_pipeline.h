/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_VK_PIPELINE_H
#define MIRENDERER_VK_PIPELINE_H

#include "vk_rhi.h"
#include "rhi/rhi_pipeline.h"
#include "rhi/rhi_desc.h"

MI_NAMESPACE_BEGIN

struct VulkanPipelineBindingRemappings {
    struct RemappedDestination {
        uint32_t set;
        uint32_t binding;
    };
    FORCEINLINE void AddRemapping (RHIPipelineResourceType type, uint32_t src_slot, uint32_t dst_set, uint32_t dst_binding) {
        auto & list = lists[(uint32_t)type];
        list.insert(list.begin() + src_slot, {dst_set, dst_binding});
    }
    FORCEINLINE void Reset () {
        for (auto & list : lists) {
            list.clear();
        }
    }
    FORCEINLINE RemappedDestination GetDestination (RHIPipelineResourceType type, uint32_t src_slot) const {
        return lists[(uint32_t)type][src_slot];
    }

    std::vector<RemappedDestination> lists[(uint32_t)RHIPipelineResourceType::kMax];
};

class VulkanGraphicsPipeline : public RHIGraphicsPipeline {
public:
    using RHIGraphicsPipeline::RHIGraphicsPipeline;

    FORCEINLINE vk::Pipeline GetPipeline() const { return vk_pipeline_; }
    FORCEINLINE vk::PipelineLayout GetPipelineLayout() const { return vk_pipeline_layout_; }
    FORCEINLINE vk::DescriptorSetLayout GetPrivateDescriptorSetLayout() const { return vk_private_descriptor_set_layout_; }
    FORCEINLINE vk::RenderPass GetRenderPass() const { return vk_render_pass_; }

    ~VulkanGraphicsPipeline();

protected:

    bool CompileRHI (const RHIGraphicsPipelineDesc &) override;
    void ResetRHI () override;
    void OnNameChanged () override;

    vk::Pipeline vk_pipeline_;
    vk::PipelineLayout vk_pipeline_layout_;
    vk::DescriptorSetLayout vk_private_descriptor_set_layout_;
    vk::RenderPass vk_render_pass_;

    // The remapping info for the "bindfull" resources
    VulkanPipelineBindingRemappings remappings_;
    // Bindless resources are directly mapped via their identity indices within the bindless table uniform buffer.
    // so no need to keep track of their mappings.

    // Aligned size of the push constant range
    uint32_t push_constant_roundup_size_ {};
};

class VulkanComputePipeline : public RHIComputePipeline {
public:
    using RHIComputePipeline::RHIComputePipeline;

    FORCEINLINE vk::Pipeline GetPipeline() const { return vk_pipeline_; }
    FORCEINLINE vk::PipelineLayout GetPipelineLayout() const { return vk_pipeline_layout_; }
    FORCEINLINE vk::DescriptorSetLayout GetPrivateDescriptorSetLayout() const { return vk_private_descriptor_set_layout_; }

    ~VulkanComputePipeline();

protected:

    bool CompileRHI (RHIShader * ) override;
    void ResetRHI () override;
    void OnNameChanged () override;

    struct BindingRemappingInfo {
        uint32_t dst_set;
        uint32_t dst_binding;
        uint32_t src_slot;
        RHIPipelineResourceType src_type;
    };

    vk::Pipeline vk_pipeline_;
    vk::PipelineLayout vk_pipeline_layout_;

    // The private descriptor set layout for this pipeline
    vk::DescriptorSetLayout vk_private_descriptor_set_layout_;

    // The remapping info for the "bindfull" resources
    VulkanPipelineBindingRemappings remappings_;
    // Bindless resources are directly mapped via their identity indices within the bindless table uniform buffer.
    // so no need to keep track of their mappings.

    // Aligned size of the push constant range
    uint32_t push_constant_roundup_size_ {};
};

MI_NAMESPACE_END

#endif //MIRENDERER_VK_PIPELINE_H
