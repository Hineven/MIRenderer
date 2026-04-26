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
#include "vk_root_signature.h"

MI_NAMESPACE_BEGIN

class RHIPipelineRootSignature;

class VulkanGraphicsPipeline : public RHIGraphicsPipeline {
public:
    using RHIGraphicsPipeline::RHIGraphicsPipeline;

    FORCEINLINE vk::Pipeline GetPipeline() const { return vk_pipeline_; }
    FORCEINLINE vk::PipelineLayout GetPipelineLayout() const { return vk_pipeline_layout_; }
    RHIPipelineRootSignature * GetRootSignature() const override { return root_signature_; }

    void SetName(const std::string &name) override;
    ~VulkanGraphicsPipeline();

    void *GetAPIHandle() const override;

protected:

    bool CompileRHI (const RHIGraphicsPipelineDesc &, RHIPipelineRootSignature * root) override;
    void ResetRHI () override;

    vk::Pipeline vk_pipeline_;
    vk::PipelineLayout vk_pipeline_layout_;
    vk::DescriptorSetLayout vk_private_descriptor_set_layout_;

    uint32_t push_constant_roundup_size_ {};

    VulkanRootSignature * root_signature_ {};
};

class VulkanComputePipeline : public RHIComputePipeline {
public:
    using RHIComputePipeline::RHIComputePipeline;

    FORCEINLINE vk::Pipeline GetPipeline() const { return vk_pipeline_; }
    FORCEINLINE vk::PipelineLayout GetPipelineLayout() const { return vk_pipeline_layout_; }
    RHIPipelineRootSignature * GetRootSignature() const override { return root_signature_; }

    void SetName (const std::string & name) override;
    ~VulkanComputePipeline();

    void *GetAPIHandle() const override;

protected:

    bool CompileRHI (RHIShader * , RHIPipelineRootSignature * root) override;
    void ResetRHI () override;

    struct BindingRemappingInfo {
        uint32_t dst_set;
        uint32_t dst_binding;
        uint32_t src_slot;
        RHIPipelineResourceType src_type;
    };

    vk::Pipeline vk_pipeline_;
    vk::PipelineLayout vk_pipeline_layout_;

    vk::DescriptorSetLayout vk_private_descriptor_set_layout_;

    uint32_t push_constant_roundup_size_ {};

    VulkanRootSignature * root_signature_ {};
};

class VulkanRayTracingPipeline : public RHIRayTracingPipeline {
public:
    using RHIRayTracingPipeline::RHIRayTracingPipeline;

    FORCEINLINE vk::Pipeline GetPipeline() const { return vk_pipeline_; }
    FORCEINLINE vk::PipelineLayout GetPipelineLayout() const { return vk_pipeline_layout_; }
    RHIPipelineRootSignature * GetRootSignature() const override { return root_signature_; }

    // RHIRayTracingPipeline interface
    uint32_t GetShaderGroupHandleSize() const override;
    bool GetShaderGroupHandles(uint32_t first_group, uint32_t group_count, void* data) const override;
    uint32_t GetShaderGroupHandleAlignment() const override;
    uint32_t GetShaderGroupBaseAlignment() const override;

    // SBT stride methods implementation
    uint32_t GetRaygenSBTStride() const override;
    uint32_t GetMissSBTStride() const override;
    uint32_t GetHitSBTStride() const override;
    uint32_t GetCallableSBTStride() const override;

    void SetName(const std::string& name) override;
    ~VulkanRayTracingPipeline();

    void* GetAPIHandle() const override;

protected:
    bool CompileRHI(const RHIRayTracingPipelineDesc& desc, RHIPipelineRootSignature * root) override;
    void ResetRHI() override;

private:
    vk::Pipeline vk_pipeline_;
    vk::PipelineLayout vk_pipeline_layout_;
    vk::DescriptorSetLayout vk_private_descriptor_set_layout_;

    // Ray tracing specific properties
    uint32_t shader_group_handle_size_ = 0;
    uint32_t shader_group_handle_alignment_ = 0;
    uint32_t shader_group_base_alignment_ = 0;

    // SBT stride values (calculated during compilation)
    uint32_t raygen_sbt_stride_ = 0;
    uint32_t miss_sbt_stride_ = 0;
    uint32_t hit_sbt_stride_ = 0;
    uint32_t callable_sbt_stride_ = 0;

    uint32_t push_constant_roundup_size_ = 0;

    VulkanRootSignature * root_signature_ {};
};

MI_NAMESPACE_END

#endif //MIRENDERER_VK_PIPELINE_H
