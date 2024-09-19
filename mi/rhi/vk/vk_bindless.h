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

    friend class VulkanRHI;
protected:

    void Initialize_RHIThread () ;
    void Destroy_RHIThread () ;

    void FreeResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) override;
    void CommitResourceSlotUpdateRHI (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) override;


    vk::DescriptorSetLayout bindless_descriptor_set_layout_;
    int set_index_ = 0;
    vk::DescriptorSet       bindless_descriptor_sets_[2];
    vk::DescriptorPool      bindless_descriptor_pool_;

    enum BindlessResourceFlagBits {
        kReadOnly = 1u<<0,
        kReadWrite = 1u<<1,
    };

    struct {
        vk::Sampler linear_wrap;        // 0
        vk::Sampler linear_clamp_edge;  // 1
        vk::Sampler nearest_wrap;       // 2
        vk::Sampler nearest_clamp_edge; // 3
    } immutable_samplers_;

    std::byte update_descriptor_set_buffer[
            C::kMaxNumBindlessResourceSlotsPerChannel
            * std::max(sizeof(vk::DescriptorImageInfo), sizeof(vk::DescriptorBufferInfo))
    ];

};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_VK_BINDLESS_H
