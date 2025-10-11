/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "vk_resource.h"

#include "vk_as.h"
#include "vk_conversion.h"

MI_NAMESPACE_BEGIN

VulkanSampler::VulkanSampler(RHISamplerDesc desc) :
RHISampler(desc) {
    auto device = GetVulkanRHI()->GetDevice();
    auto vk_border_color = vk::BorderColor::eFloatOpaqueBlack;
    if (desc.border_color[0] == 0.0f && desc.border_color[1] == 0.0f &&
        desc.border_color[2] == 0.0f) {
        if (desc.border_color[3] == 0.0f)
            vk_border_color = vk::BorderColor::eFloatTransparentBlack;
        else if (desc.border_color[3] == 1.0f)
            vk_border_color = vk::BorderColor::eFloatOpaqueBlack;
        else
            vk_border_color = vk::BorderColor::eFloatCustomEXT;
    } else if (desc.border_color[0] == 1.0f && desc.border_color[1] == 1.0f &&
               desc.border_color[2] == 1.0f) {
        if (desc.border_color[3] == 1.0f)
            vk_border_color = vk::BorderColor::eFloatOpaqueWhite;
        else
            vk_border_color = vk::BorderColor::eFloatCustomEXT;
    }
    if (vk_border_color == vk::BorderColor::eFloatCustomEXT) {
        mi_check(false, "Not supported yet");
    }
    vk_sampler_ = device.createSampler(
        vk::SamplerCreateInfo()
        .setMagFilter(GetVulkanFilter(desc.min_filter))
        .setMinFilter(GetVulkanFilter(desc.mag_filter))
        .setAddressModeU(GetVulkanAddressingMode(desc.address_mode_u))
        .setAddressModeV(GetVulkanAddressingMode(desc.address_mode_v))
        .setAddressModeW(GetVulkanAddressingMode(desc.address_mode_w))
        .setAnisotropyEnable(VK_FALSE)
        .setMaxAnisotropy(1)
        .setBorderColor(vk_border_color)
        .setUnnormalizedCoordinates(VK_FALSE)
        .setCompareEnable(VK_FALSE)
        .setCompareOp(vk::CompareOp::eAlways)
        .setMipmapMode(GetVulkanMipmapMode(desc.mipmap_mode))
        .setMipLodBias(0.0f)
        .setMinLod(0.0f)
        .setMaxLod(0.0f)
    );
}

VulkanSampler::~VulkanSampler() {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroySampler(vk_sampler_);
}

void * VulkanSampler::GetAPIHandle() const {
    return (void*)vk_sampler_;
}

void VulkanSampler::SetName(const std::string& name) {
    auto device = GetVulkanRHI()->GetDevice();
    vk::DebugUtilsObjectNameInfoEXT name_info{
        vk::ObjectType::eSampler,
        reinterpret_cast<uint64_t>(static_cast<VkSampler>(vk_sampler_)),
        name.c_str()
    };
    device.setDebugUtilsObjectNameEXT(name_info);
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
    auto ret = dev.waitForFences({vk_fence_}, VK_TRUE, 500 * 1000 * 1000); // 500 ms timeout
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
    CHECK_RHI_THREAD();
    assert(can_be_waited_);
    submission_sem_.release();
}

void *VulkanSyncPoint::GetAPIHandle() const {
    return (void*)vk_fence_;
}

void VulkanSyncPoint::SetName(const std::string& name) {
    GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT()
        .setObjectType(vk::ObjectType::eFence)
        .setObjectHandle(reinterpret_cast<uint64_t>(static_cast<VkFence>(vk_fence_)))
        .setPObjectName(name.c_str())
    );
}

VulkanTimestamp::~VulkanTimestamp() {

}

uint64_t VulkanTimestamp::QueryTimestamp() const {
    auto rhi = GetVulkanRHI();
    struct TimestampValue {
        char padding[16]; // maximum of 128 bytes
    };
    auto result = rhi->GetDevice().getQueryPoolResult<uint64_t>(
        rhi->GetTimestampQueryPool(), query_index_, 1,
        sizeof(TimestampValue),
        vk::QueryResultFlagBits::eWait | vk::QueryResultFlagBits::eWithAvailability
    );
    uint64_t value = *(uint64_t*)&result;
    auto valid_bits = std::min(rhi->GetDeviceProperties().timestamp_valid_bits, 64u);
    if (valid_bits < 64) {
        uint64_t mask = (1ull << valid_bits) - 1;
        value &= mask;
    }
    return value;
}



MI_NAMESPACE_END