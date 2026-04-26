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

struct VulkanPipelineBindingRemappings {
    struct RemappedDestination {
        uint32_t set;
        uint32_t binding;
    };
    FORCEINLINE void AddRemapping (RHIPipelineResourceType type, uint32_t src_slot, uint32_t dst_set, uint32_t dst_binding) {
        auto & list = lists[(uint32_t)type];
        mi_assert(src_slot <= list.size(), "AddRemapping: out-of-order insertion, src_slot={}, list.size()={}", src_slot, list.size());
        list.insert(list.begin() + src_slot, {dst_set, dst_binding});
    }
    FORCEINLINE void Reset () {
        for (auto & list : lists) {
            list.clear();
        }
    }
    FORCEINLINE RemappedDestination GetDestination (RHIPipelineResourceType type, uint32_t src_slot) const {
        if (lists[(uint32_t)type].size() > src_slot) {
            return lists[(uint32_t)type][src_slot];
        }
        return {UINT32_MAX, UINT32_MAX}; // Invalid destination
    }

    std::vector<RemappedDestination> lists[(uint32_t)RHIPipelineResourceType::kMax];
};

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

    FORCEINLINE const VulkanPipelineBindingRemappings & GetRemappings() const { return remappings_; }

private:
    vk::DescriptorSetLayout descriptor_set_layout_ {};
    vk::PipelineLayout pipeline_layout_ {};
    vk::Device device_ {};

    VulkanPipelineBindingRemappings remappings_;
    std::vector<uint32_t> owned_crc_data_;
};

MI_NAMESPACE_END

#endif // MI_VK_ROOT_SIGNATURE_H
