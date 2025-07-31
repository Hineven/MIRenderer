/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_buffer.h"
#include "vk_conversion.h"

MI_NAMESPACE_BEGIN

VulkanBuffer::VulkanBuffer(RHIBufferDesc desc)
        : RHIBuffer(desc) {
    vk::BufferCreateInfo buffer_info;
    buffer_info.size = desc.size;
    buffer_info.usage = GetVulkanBufferUsage(desc.usage);
    buffer_info.sharingMode = vk::SharingMode::eExclusive;

    vma::AllocationCreateFlags alloc_flags {};
    if(desc.usage & RHIBufferUsageFlagBits::kStaging) {
        alloc_flags |= vma::AllocationCreateFlagBits::eHostAccessSequentialWrite;
    }
    if(desc.usage & RHIBufferUsageFlagBits::kReadback) {
        alloc_flags = vma::AllocationCreateFlagBits::eHostAccessRandom;
    }
    vma::AllocationCreateInfo alloc_info {
            alloc_flags,
            vma::MemoryUsage::eAuto
    };
    auto & vma = GetVulkanRHI()->GetVmaAllocator();
    auto result = vma.createBuffer(buffer_info, alloc_info);
    if (GetName() && result.second) vma.setAllocationName(result.second, GetName());
    vk_buffer_ = result.first;
    allocation_ = result.second;
    mi_assert(vk_buffer_ && allocation_, "Failed to allocate buffer!");
}

void *VulkanBuffer::Map() {
    if(!(desc_.usage & RHIBufferUsageFlagBits::kStaging) && !(desc_.usage & RHIBufferUsageFlagBits::kReadback)) {
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
        VulkanBuffer::Unmap();
    }
    GetVulkanRHI()->GetVmaAllocator().destroyBuffer(vk_buffer_, allocation_);
}

void *VulkanBuffer::GetAPIHandle() const {
    return (void*)vk_buffer_;
}

void VulkanBuffer::SetName(const std::string & name) {
    RHIBuffer::SetName(name);
    GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT {
            vk::ObjectType::eBuffer,
            reinterpret_cast<uint64_t>((VkBuffer)vk_buffer_),
            name.c_str()
        }
    );
    if (allocation_) {
        GetVulkanRHI()->GetVmaAllocator().setAllocationName(allocation_, GetName());
    }
}

uint64_t VulkanBuffer::GetDeviceAddress() const {
    return GetVulkanRHI()->GetDevice().getBufferAddress(vk_buffer_);
}


MI_NAMESPACE_END