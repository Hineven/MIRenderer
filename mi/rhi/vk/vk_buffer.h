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
    VulkanBuffer(size_t buffer_size, RHIBufferUsageFlags usage);
    virtual ~VulkanBuffer() override;

    vk::Buffer GetBuffer() const { return vk_buffer_; }

    void * Map() override;
    void Unmap() override;

    // Emit a memory barrier for the buffer.
    FORCEINLINE void MemBarrier (
            vk::CommandBuffer cmd,
            vk::PipelineStageFlags src_stages, vk::PipelineStageFlags dst_stages,
            vk::AccessFlags src_access, vk::AccessFlags dst_access,
            size_t offset = 0, size_t size = VK_WHOLE_SIZE) {
        vk::BufferMemoryBarrier barrier;
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = vk_buffer_;
        barrier.offset = offset;
        barrier.size = size;
        cmd.pipelineBarrier(src_stages, dst_stages, {}, nullptr, barrier, nullptr);
    }

protected:
    vk::Buffer vk_buffer_;
    vma::Allocation allocation_;

    void * mapped_ptr_;
};

MI_NAMESPACE_END

#endif //MIRENDERER_VK_BUFFER_H
