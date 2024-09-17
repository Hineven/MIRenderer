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

    ~VulkanBindlessManager() ;

    // Mark and potentially transit layouts for the used resource.
    void UseResource (vk::CommandBuffer cmd, int slot_index, vk::PipelineStageFlags use_stages) ;

    friend class VulkanRHI;
protected:

    void Initialize_RHIThread () ;
    void Destroy_RHIThread () ;

    void UpdateResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot) override;

    vk::DescriptorSetLayout bindless_descriptor_set_layout_;
};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_VK_BINDLESS_H
