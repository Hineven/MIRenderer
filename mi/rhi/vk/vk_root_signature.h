/*
 * Created: 2026/4/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VK_ROOT_SIGNATURE_H
#define MI_VK_ROOT_SIGNATURE_H

#include <vulkan/vulkan.hpp>
#include "rhi/rhi_root_signature.h"
#include "rhi/rhi_desc.h"
#include "core/platform.h"
#include "core/common.h"

MI_NAMESPACE_BEGIN

// Vulkan implementation of RHIPipelineRootSignature.
// Owns a vk::DescriptorSetLayout (set 0, for per-pipeline bindfull resources) and a vk::PipelineLayout.
// Set 1 is always the bindless descriptor set layout from VulkanBindlessManager.
// CRC name data is copied into owned_crc_data_ to ensure lifetime independence from the caller.
class VulkanRootSignature : public RHIPipelineRootSignature {
public:
    VulkanRootSignature(const RHIPipelineRootSignatureDesc & desc, vk::Device device);
    ~VulkanRootSignature() override;

    void *GetAPIHandle() const override;

    FORCEINLINE vk::PipelineLayout GetPipelineLayout() const { return pipeline_layout_; }
    FORCEINLINE vk::DescriptorSetLayout GetDescriptorSetLayout() const { return descriptor_set_layout_; }

private:
    vk::DescriptorSetLayout descriptor_set_layout_ {};
    vk::PipelineLayout pipeline_layout_ {};
    vk::Device device_ {};

    std::vector<uint32_t> owned_crc_data_;
};

MI_NAMESPACE_END

#endif // MI_VK_ROOT_SIGNATURE_H
