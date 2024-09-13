/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_VK_BUFFER_H
#define MIRENDERER_VK_BUFFER_H

#include "rhi/rhi_buffer.h"
#include "vk_rhi.h"

MI_NAMESPACE_BEGIN

class VulkanBuffer : public RHIBuffer {
public:
    VulkanBuffer(size_t buffer_size, RHIBufferUsageFlags usage, RHIGPUAccessFlagBits access);
    virtual ~VulkanBuffer() override;

    vk::Buffer GetBuffer() const { return vk_buffer_; }

    void * Map() override;
    void Unmap() override;

    FORCEINLINE void Use (vk::CommandBuffer cmd, vk::PipelineStageFlags use_stage, vk::AccessFlags use_access) {
        if(!(use_access & vk::AccessFlagBits::eMemoryWrite)) {
            // R-R, no barrier needed
            if(!(using_accesses_ & vk::AccessFlagBits::eMemoryWrite)) {
                using_accesses_ |= use_access;
                using_stages_ |= use_stage;
                return ;
            }
        }
        vk::BufferMemoryBarrier barrier;
        barrier.srcAccessMask = using_accesses_;
        barrier.dstAccessMask = use_access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = vk_buffer_;
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
        cmd.pipelineBarrier(using_stages_, use_stage, {}, nullptr, barrier, nullptr);
        using_accesses_ = use_access;
        using_stages_ = use_stage;
    }

protected:
    vk::Buffer vk_buffer_;
    vk::PipelineStageFlags using_stages_;
    vk::AccessFlags using_accesses_;
    vma::Allocation allocation_;

    void * mapped_ptr_;
};

MI_NAMESPACE_END

#endif //MIRENDERER_VK_BUFFER_H
