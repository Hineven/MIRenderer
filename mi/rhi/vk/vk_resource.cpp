/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "vk_resource.h"

#include <ranges>

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
    RHIResource::SetName(name);
#ifndef NDEBUG
    auto device = GetVulkanRHI()->GetDevice();
    vk::DebugUtilsObjectNameInfoEXT name_info{
        vk::ObjectType::eSampler,
        reinterpret_cast<uint64_t>(static_cast<VkSampler>(vk_sampler_)),
        name.c_str()
    };
    device.setDebugUtilsObjectNameEXT(name_info);
#endif
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
    [[maybe_unused]] auto ret = dev.waitForFences({vk_fence_}, VK_TRUE, 500 * 1000 * 1000); // 500 ms timeout
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
    RHIResource::SetName(name);
#ifndef NDEBUG
    GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT()
        .setObjectType(vk::ObjectType::eFence)
        .setObjectHandle(reinterpret_cast<uint64_t>(static_cast<VkFence>(vk_fence_)))
        .setPObjectName(name.c_str())
    );
#endif
}

VulkanTimestamp::~VulkanTimestamp() {

}

uint64_t VulkanTimestamp::QueryTimestamp() const {
#if ENABLE_TIMESTAMP
    auto rhi = GetVulkanRHI();
    uint64_t value = 0;
    // Wait for the timestamp to be available and read it as 64-bit
    auto res = rhi->GetDevice().getQueryPoolResults(
        rhi->GetTimestampQueryPool(),
        query_index_,
        1,
        sizeof(uint64_t),
        &value,
        sizeof(uint64_t),
        vk::QueryResultFlagBits::eWait | vk::QueryResultFlagBits::e64
    );
    mi_warning(res == vk::Result::eSuccess, "Failed to get timestamp query result ({}).", vk::to_string(res));
    if (res != vk::Result::eSuccess) return UINT64_MAX;
    // Mask to the hardware-supported valid bits
    auto valid_bits = std::min(rhi->GetDeviceProperties().timestamp_valid_bits, 64u);
    if (valid_bits < 64) {
        const uint64_t mask = (valid_bits == 0) ? 0ull : ((1ull << valid_bits) - 1ull);
        if (value != UINT64_MAX) value &= mask;
    }
    return value;
#else
    mi_warning(true, "Querying timestamp returns UINT64_MAX when ENABLE_TIMESTAMP is disabled.");
    return UINT64_MAX;
#endif
}

std::vector<uint64_t> VulkanRHI::QueryTimestamps(std::span<RHITimestamp *> timestamps) {
#if ENABLE_TIMESTAMP
    auto rhi = GetVulkanRHI();
    // Check for continuous query indices
    std::vector<uint32_t> query_indices, query_rank_indirection;
    query_indices.reserve(timestamps.size());
    query_rank_indirection.reserve(timestamps.size());
    for (auto [i, ts] : std::views::enumerate(timestamps)) {
        auto vk_ts = static_cast<VulkanTimestamp *>(ts);
        query_indices.push_back(vk_ts->query_index_);
        query_rank_indirection.push_back((uint32_t)i);
    }
    // Indirect sorting
    std::sort(query_rank_indirection.begin(), query_rank_indirection.end(),
        [&](uint32_t a, uint32_t b) {
            return query_indices[a] < query_indices[b];
        }
    );
    // Check for continuity
    uint32_t continuous_start = 0;
    std::vector<std::pair<uint32_t, uint32_t>> continuous_ranges;
    for (size_t i = 1; i < query_indices.size(); i++) {
        if (query_indices[query_rank_indirection[i]] != query_indices[query_rank_indirection[i - 1]] + 1) {
            // Break in continuity
            continuous_ranges.push_back({continuous_start, (uint32_t)(i - 1)});
            continuous_start = (uint32_t)i;
        }
    }
    continuous_ranges.push_back({continuous_start, (uint32_t)(query_indices.size() - 1)});
    std::vector<uint64_t> results(timestamps.size());
    // Query is performed in RHI thread.
    auto query_task = EnqueueRHIThreadTask([&]() {
        for (auto r : continuous_ranges) {
            // Wait for the timestamp to be available and read it as 64-bit
            auto count = r.second - r.first + 1;
            auto values = std::vector<uint64_t>(count);
            auto res = rhi->GetDevice().getQueryPoolResults(
                rhi->GetTimestampQueryPool(),
                query_indices[query_rank_indirection[r.first]],
                count,
                sizeof(uint64_t) * count,
                values.data(),
                sizeof(uint64_t),
                vk::QueryResultFlagBits::eWait | vk::QueryResultFlagBits::e64
            );
            mi_warning(res == vk::Result::eSuccess, "Failed to get timestamp query result ({}).", vk::to_string(res));
            if (res == vk::Result::eSuccess) {
                // Scatter to results
                for (size_t i = 0; i < count; i++) {
                    auto original_rank = query_rank_indirection[r.first + i];
                    results[original_rank] = values[i];
                }
            } else {
                // Fill with UINT64_MAX on failure
                for (size_t i = 0; i < count; i++) {
                    auto original_rank = query_rank_indirection[r.first + i];
                    results[original_rank] = UINT64_MAX;
                }
            }
        }
    });
    // Wait for completion
    query_task.wait();
    // Mask to the hardware-supported valid bits
    auto valid_bits = std::min(rhi->GetDeviceProperties().timestamp_valid_bits, 64u);
    if (valid_bits < 64) {
        const uint64_t mask = (valid_bits == 0) ? 0ull : ((1ull << valid_bits) - 1ull);
        for (auto & value : results) {
            if (value != UINT64_MAX) value &= mask;
        }
    }
    return results;
#else
    mi_warning(true, "Querying timestamp returns UINT64_MAX when ENABLE_TIMESTAMP is disabled.");
    auto result = std::vector<uint64_t>{};
    result.resize(timestamps.size(), UINT64_MAX);
    return result;
#endif
}



MI_NAMESPACE_END