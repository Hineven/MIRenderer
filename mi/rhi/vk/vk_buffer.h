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

class VulkanBuffer final : public RHIBuffer {
public:
    VulkanBuffer(RHIBufferDesc desc);
    virtual ~VulkanBuffer() override;

    vk::Buffer GetBuffer() const { return vk_buffer_; }

    void * Map() override;
    void Unmap() override;

    void * GetAPIHandle() const override;

    void SetName(const std::string & name) override;

    uint64_t GetDeviceAddress() const override ;

protected:
    vk::Buffer vk_buffer_;
    vma::Allocation allocation_;

    void * mapped_ptr_;

private:
    uint64_t cached_device_address_ {UINT64_MAX};
};

MI_NAMESPACE_END

#endif //MIRENDERER_VK_BUFFER_H
