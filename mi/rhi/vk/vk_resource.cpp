/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "vk_resource.h"
#include "vk_conversion.h"

MI_NAMESPACE_BEGIN

VulkanSampler::VulkanSampler(RHISamplerFilterType filter, RHISamplerAddressModeType addressing) :
RHISampler(filter, addressing) {
    auto device = GetVulkanRHI()->GetDevice();
    auto vk_filter = GetVulkanFilter(filter);
    auto vk_addressing = GetVulkanAddressingMode(addressing);
    vk_sampler_ = device.createSampler(
        vk::SamplerCreateInfo()
        .setMagFilter(vk_filter)
        .setMinFilter(vk_filter)
        .setAddressModeU(vk_addressing)
        .setAddressModeV(vk_addressing)
        .setAddressModeW(vk_addressing)
        .setAnisotropyEnable(VK_FALSE)
        .setMaxAnisotropy(1)
        .setBorderColor(vk::BorderColor::eFloatOpaqueBlack)
        .setUnnormalizedCoordinates(VK_FALSE)
        .setCompareEnable(VK_FALSE)
        .setCompareOp(vk::CompareOp::eAlways)
        .setMipmapMode(vk::SamplerMipmapMode::eLinear)
        .setMipLodBias(0.0f)
        .setMinLod(0.0f)
        .setMaxLod(0.0f)
    );
}

VulkanSampler::~VulkanSampler() {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroySampler(vk_sampler_);
}

VulkanSyncPoint::VulkanSyncPoint() {
    auto device = GetVulkanRHI()->GetDevice();
    vk_fence_ = device.createFence({});
}

void VulkanSyncPoint::Wait() {
    assert(IsRenderThread());
    assert(can_be_waited_);
    // Wait for command buffer submission first.
    submission_sem_.acquire();
    auto dev = GetVulkanRHI()->GetDevice();
    auto ret = dev.waitForFences({vk_fence_}, VK_TRUE, UINT64_MAX);
    mi_assert(ret == vk::Result::eSuccess, "Failed to wait for fence.");
    can_be_waited_ = false;
}

VulkanSyncPoint::~VulkanSyncPoint() {
    auto dev = GetVulkanRHI()->GetDevice();
    dev.destroy(vk_fence_);
}

void VulkanSyncPoint::Reset() {
    assert(IsRenderThread());
    auto dev = GetVulkanRHI()->GetDevice();
    dev.resetFences({vk_fence_});
    can_be_waited_ = true;
}

void VulkanSyncPoint::NotifySubmission() {
    assert(IsRHIThread());
    assert(can_be_waited_);
    submission_sem_.release();
}

MI_NAMESPACE_END