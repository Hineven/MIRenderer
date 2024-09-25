/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_VK_RESOURCE_H
#define MIRENDERER_VK_RESOURCE_H

#include <semaphore>
#include "vk_rhi.h"
#include "rhi/rhi_resource.h"

MI_NAMESPACE_BEGIN

class VulkanSampler : public RHISampler {
public:
    VulkanSampler(RHISamplerFilterType filter, RHISamplerAddressModeType addressing);
    ~VulkanSampler() override;

    FORCEINLINE vk::Sampler GetSampler() const { return vk_sampler_; }
protected:
    vk::Sampler vk_sampler_;
};

class VulkanAccelerationStructure : public RHIResource {
public:
    VulkanAccelerationStructure(vk::AccelerationStructureKHR as) : as_(as) {}
    ~VulkanAccelerationStructure() override {
        auto device = GetVulkanRHI()->GetDevice();
        device.destroyAccelerationStructureKHR(as_);
    }

    FORCEINLINE vk::AccelerationStructureKHR GetAccelerationStructure() const { return as_; }

//    void Use (vk::CommandBuffer cmd, vk::PipelineStageFlags use_stages, vk::AccessFlags use_access) ;
protected:
    vk::AccelerationStructureKHR as_;
};

class VulkanSyncPoint : public RHISyncPoint {
protected:
    VulkanSyncPoint () ;
    ~VulkanSyncPoint () override;
public:
    void Wait () override;
    void Reset () override;
    void NotifySubmission () override;
    FORCEINLINE vk::Fence GetFence () const {
        return vk_fence_;
    }
    friend class VulkanRHI;
protected:
    std::binary_semaphore submission_sem_ {0};
    vk::Fence vk_fence_ {};
    bool can_be_waited_ {true};
};

MI_NAMESPACE_END

#endif //MIRENDERER_VK_RESOURCE_H
