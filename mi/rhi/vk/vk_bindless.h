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

    vk::DescriptorSetLayout GetBindlessDescriptorSetLayout();

    vk::DescriptorSet       GetBindlessDescriptorSet();

    // Place barriers for bindless resources
    void UseBindlessResource (uint32_t bindless_slot, vk::CommandBuffer cmd, vk::PipelineStageFlags use_stages, RHIGPUAccessFlags use_access);


protected:
    friend class VulkanRHI;

    VulkanBindlessManager() ;
    ~VulkanBindlessManager() ;

    void UpdateResourceSlotRHI(RHIBindlessResourceType type, uint32_t slot) override;

    vk::DescriptorSetLayout bindless_descriptor_set_layout_;
};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_VK_BINDLESS_H
