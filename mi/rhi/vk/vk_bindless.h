/*
 * Created: 2024/7/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_VK_BINDLESS_H
#define MIRENDERERDEV_VK_BINDLESS_H

#include "vk_rhi.h"
#include "../include/rhi/rhi_bindless.h"

MI_NAMESPACE_BEGIN

class VulkanBindlessManager : public RHIBindlessManager {
public:
    VulkanBindlessManager() ;

    vk::DescriptorSetLayout GetBindlessDescriptorSetLayout();

    vk::DescriptorSet       GetBindlessDescriptorSet();

    void SwapSets_RHIThread (std::span<RHIPackedBindlessSlot> slots_to_free) override;

    ~VulkanBindlessManager() ;

    friend class VulkanRHI;
protected:

    void Initialize_RHIThread () ;
    void Destroy_RHIThread () ;

    void FreeResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) override;
    void CommitResourceSlotUpdateRHI (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) override;


    vk::DescriptorSetLayout bindless_descriptor_set_layout_;
    // Read / updated in RHI thread only
    int set_index_ = 0;
    vk::DescriptorSet       bindless_descriptor_sets_[2];
    vk::DescriptorPool      bindless_descriptor_pool_;

    struct {
        vk::Sampler linear_wrap;        // 0
        vk::Sampler linear_clamp_edge;  // 1
        vk::Sampler nearest_wrap;       // 2
        vk::Sampler nearest_clamp_edge; // 3
    } immutable_samplers_;

};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_VK_BINDLESS_H
