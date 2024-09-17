/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_buffer.h"
#include "vk_conversion.h"

MI_NAMESPACE_BEGIN

VulkanBuffer::VulkanBuffer(size_t buffer_size, RHIBufferUsageFlags usage, RHIGPUAccessFlagBits access)
        : RHIBuffer(buffer_size, usage, access) {
    vk::BufferCreateInfo buffer_info;
    buffer_info.size = buffer_size;
    buffer_info.usage = GetVulkanBufferUsage(usage);
    buffer_info.sharingMode = vk::SharingMode::eExclusive;
    auto result = GetVulkanRHI()->GetDevice().createBuffer(&buffer_info, nullptr, &vk_buffer_);
    if(result != vk::Result::eSuccess) {
        // TODO move this out of the constructor
        mi_assert(false, "Failed to create buffer!");
    }

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
    allocation_ = GetVulkanRHI()->GetVmaAllocator().allocateMemoryForBuffer(vk_buffer_, alloc_info);
    if(!allocation_) {
        mi_assert(false, "Failed to allocate memory for buffer!");
    }
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
    GetVulkanRHI()->GetVmaAllocator().freeMemory(allocation_);
    GetVulkanRHI()->GetDevice().destroyBuffer(vk_buffer_);
}

MI_NAMESPACE_END