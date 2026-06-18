/*
 * Created: 2026/6/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef VK_PTLAS_H
#define VK_PTLAS_H

#include "rhi/rhi_ptlas.h"
#include "vk_resource.h"
#include "vk_buffer.h"
#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

MI_NAMESPACE_BEGIN

// Vulkan backend implementation of RHIPartitionedTLAS.
//
// A PTLAS is NOT a Vulkan object — it has no VkAccelerationStructureKHR handle.
// It is a device-addressable buffer whose contents are produced by
// vkCmdBuildPartitionedAccelerationStructuresNV. This class owns that backing
// buffer (allocated via VMA) and exposes its device address for descriptor binding.
class VulkanPartitionedTLAS : public RHIPartitionedTLAS {
public:
    VulkanPartitionedTLAS();
    ~VulkanPartitionedTLAS() override;

    RHIPartitionedTLASBuildSizes GetBuildSizes(
        const RHIPartitionedTLASInstancesInput& input) const override;

    bool Allocate(size_t size, const RHIPartitionedTLASInstancesInput& config) override;

    uint64_t GetDeviceAddress() const override;
    size_t GetSize() const override { return size_; }

    const RHIPartitionedTLASInstancesInput& GetConfig() const override { return config_; }

    void SetName(const std::string& name) override;

    // Vulkan-specific accessors (for barrier and build command implementation)
    FORCEINLINE vk::Buffer GetBuffer() const { return buffer_; }

    FORCEINLINE void * GetAPIHandle() const override { return (void*)buffer_; }

private:
    void ResetRHI();

    vk::Buffer       buffer_;
    vma::Allocation  allocation_ = nullptr;
    size_t           size_       = 0;
    RHIPartitionedTLASInstancesInput config_ {};
};

MI_NAMESPACE_END

#endif //VK_PTLAS_H
