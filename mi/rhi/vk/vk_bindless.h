/*
 * Created: 2024/7/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_VK_BINDLESS_H
#define MIRENDERERDEV_VK_BINDLESS_H

#include "vk_rhi.h"
#include "../rhi_bindless.h"

MI_NAMESPACE_BEGIN

class VulkanBindlessManager : public RHIBindlessManager {
public:
    VulkanBindlessManager() ;

    vk::DescriptorSetLayout GetBindlessDescriptorSetLayout();

    vk::DescriptorSet       GetBindlessDescriptorSet();

    void SwapSets_RHIThread () override;

    ~VulkanBindlessManager() ;

    // Mark and potentially transit layouts for the used resource.
    void UseResource (vk::CommandBuffer cmd, RHIBindlessResourceType type, int slot_index, vk::PipelineStageFlags use_stages) ;

    friend class VulkanRHI;
protected:

    void Initialize_RHIThread () ;
    void Destroy_RHIThread () ;

    void UpdateResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot) override;

    vk::DescriptorSetLayout bindless_descriptor_set_layout_;
    int set_index_ = 0;
    vk::DescriptorSet       bindless_descriptor_sets_[2];
    vk::DescriptorPool      bindless_descriptor_pool_;

    enum BindlessResourceFlagBits {
        kReadOnly = 1u<<0,
        kReadWrite = 1u<<1,
    };

    // offset + slot = real binding index in the descriptor set.
    uint32_t channel_binding_offsets_ [static_cast<int>(RHIBindlessResourceType::kMax) + 1];

    struct {
        vk::Sampler linear_wrap;        // 0
        vk::Sampler linear_clamp_edge;  // 1
        vk::Sampler nearest_wrap;       // 2
        vk::Sampler nearest_clamp_edge; // 3
    } immutable_samplers_;
};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_VK_BINDLESS_H
