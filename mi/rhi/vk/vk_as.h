/*
 * Created: 2025/1/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef VK_AS_H
#define VK_AS_H

#include "rhi/rhi_as.h"
#include "vk_resource.h"
#include "vk_buffer.h"
#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

MI_NAMESPACE_BEGIN

class VulkanAccelerationStructure : public RHIAccelerationStructure {
public:
    VulkanAccelerationStructure(RHIAccelerationStructureType type);
    virtual ~VulkanAccelerationStructure();

    // Create acceleration structure with given size
    bool Create(size_t size);

    // RHIAccelerationStructure interface
    uint64_t GetDeviceAddress() const override;
    RHIAccelerationStructureBuildSizesInfo GetBuildSizes(
        const RHIAccelerationStructureBuildGeometryInfo& build_info) const override;

    FORCEINLINE  void *GetAPIHandle() const override {
        return (void*)acceleration_structure_;
    }

    // Vulkan specific
    FORCEINLINE vk::AccelerationStructureKHR GetAccelerationStructure() const { return acceleration_structure_; }
    FORCEINLINE vk::Buffer GetBuffer() const { return buffer_; }

    void SetName (const std::string & name) override;

protected:
    void ResetRHI() ;

private:
    vk::AccelerationStructureKHR acceleration_structure_;
    // 使用VMA管理缓冲区内存，更简洁高效
    vk::Buffer buffer_;
    vma::Allocation allocation_;  // 使用 vma-hpp 类型而不是 C 类型
    size_t size_ = 0;
};

MI_NAMESPACE_END

#endif //VK_AS_H
