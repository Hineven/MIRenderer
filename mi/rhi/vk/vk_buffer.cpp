/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_buffer.h"
#include "vk_conversion.h"

MI_NAMESPACE_BEGIN

VulkanBuffer::VulkanBuffer(size_t buffer_size, RHIBufferUsageFlags usage)
        : RHIBuffer(buffer_size, usage) {
    vk::BufferCreateInfo buffer_info;
    buffer_info.size = buffer_size;
    buffer_info.usage = GetVulkanBufferUsage(usage);
    buffer_info.sharingMode = vk::SharingMode::eExclusive;

    vma::AllocationCreateFlags alloc_flags {};
    if(usage & RHIBufferUsageFlagBits::kStaging) {
        alloc_flags |= vma::AllocationCreateFlagBits::eHostAccessSequentialWrite;
    }
    if(usage & RHIBufferUsageFlagBits::kReadback) {
        alloc_flags = vma::AllocationCreateFlagBits::eHostAccessRandom;
    }
    vma::AllocationCreateInfo alloc_info {
            alloc_flags,
            vma::MemoryUsage::eAuto
    };
    auto result = GetVulkanRHI()->GetVmaAllocator().createBuffer(buffer_info, alloc_info);
    vk_buffer_ = result.first;
    allocation_ = result.second;
    mi_assert(vk_buffer_ && allocation_, "Failed to allocate buffer!");
}

void *VulkanBuffer::Map() {
    if(!(usage_ & RHIBufferUsageFlagBits::kStaging) && !(usage_ & RHIBufferUsageFlagBits::kReadback)) {
        mi_assert(false, "Buffer is not staging / readback buffer, cannot map!");
    }
    if(!is_mapped_) {
        auto result = GetVulkanRHI()->GetVmaAllocator().mapMemory(allocation_, &mapped_ptr_);
        if(result != vk::Result::eSuccess) {
            mi_assert(false, "Failed to map buffer memory!");
        }
        is_mapped_ = true;
    }
    return mapped_ptr_;
}

void VulkanBuffer::Unmap() {
    if (is_mapped_) {
        GetVulkanRHI()->GetVmaAllocator().unmapMemory(allocation_);
        is_mapped_ = false;
    }
}

VulkanBuffer::~VulkanBuffer() {
    if(is_mapped_) {
        Unmap();
    }
    GetVulkanRHI()->GetVmaAllocator().destroyBuffer(vk_buffer_, allocation_);
}

MI_NAMESPACE_END