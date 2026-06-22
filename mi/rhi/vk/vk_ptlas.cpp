/*
 * Created: 2026/6/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "vk_ptlas.h"
#include "vk_conversion.h"
#include "vk_rhi.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

VulkanPartitionedTLAS::VulkanPartitionedTLAS() = default;

VulkanPartitionedTLAS::~VulkanPartitionedTLAS() {
    ResetRHI();
}

RHIPartitionedTLASBuildSizes VulkanPartitionedTLAS::GetBuildSizes(
    const RHIPartitionedTLASInstancesInput& input) const {

    // VkPartitionedAccelerationStructureInstancesInputNV requires a
    // VkPartitionedAccelerationStructureFlagsNV chained via pNext. Without it the
    // driver reads undefined flag data and returns a bogus buildScratchSize (upper
    // bits corrupted). Match the NVIDIA sample, which always chains this struct
    // (even with default/zero fields).
    vk::PartitionedAccelerationStructureFlagsNV ptlas_flags{};

    vk::PartitionedAccelerationStructureInstancesInputNV vk_input{};
    vk_input.pNext = &ptlas_flags;
    vk_input.flags = GetVulkanBuildAccelerationStructureFlags(input.flags);
    vk_input.instanceCount = input.instance_count;
    vk_input.maxInstancePerPartitionCount = input.max_instance_per_partition_count;
    vk_input.partitionCount = input.partition_count;
    vk_input.maxInstanceInGlobalPartitionCount = input.max_instance_in_global_partition_count;

    auto vk_sizes = GetVulkanRHI()->GetDevice()
        .getPartitionedAccelerationStructuresBuildSizesNV(vk_input);

    RHIPartitionedTLASBuildSizes sizes{};
    sizes.acceleration_structure_size = vk_sizes.accelerationStructureSize;
    sizes.build_scratch_size = vk_sizes.buildScratchSize;
    return sizes;
}

bool VulkanPartitionedTLAS::Allocate(size_t size, const RHIPartitionedTLASInstancesInput& config) {
    if (size == 0) {
        MI_LOG(MIInfraLogType::kWarning, "Partitioned TLAS size cannot be zero");
        return false;
    }
    ResetRHI();
    config_ = config;

    auto device = GetVulkanRHI()->GetDevice();
    auto& allocator = GetVulkanRHI()->GetVmaAllocator();

    // The PTLAS backing buffer holds acceleration structure data produced by the
    // build command and is read by shaders via a PTLAS descriptor. It needs both
    // AS storage usage and a device address.
    vk::BufferCreateInfo buffer_info{};
    buffer_info.size = size;
    buffer_info.usage = vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                        vk::BufferUsageFlagBits::eShaderDeviceAddress;
    buffer_info.sharingMode = vk::SharingMode::eExclusive;

    vma::AllocationCreateInfo alloc_info{};
    alloc_info.usage = vma::MemoryUsage::eAutoPreferDevice;
    alloc_info.flags = vma::AllocationCreateFlagBits::eDedicatedMemory;
    alloc_info.requiredFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;

    auto [allocation_result, buffer_result] = allocator.createBuffer(buffer_info, alloc_info);

    buffer_ = buffer_result;
    allocation_ = allocation_result;
    size_ = size;
    return true;
}

uint64_t VulkanPartitionedTLAS::GetDeviceAddress() const {
    if (!buffer_) {
        return 0;
    }
    vk::BufferDeviceAddressInfo address_info{};
    address_info.buffer = buffer_;
    return GetVulkanRHI()->GetDevice().getBufferAddress(address_info);
}

void VulkanPartitionedTLAS::ResetRHI() {
    auto& allocator = GetVulkanRHI()->GetVmaAllocator();
    if (buffer_ && allocation_) {
        allocator.destroyBuffer(buffer_, allocation_);
        buffer_ = nullptr;
        allocation_ = nullptr;
    }
    size_ = 0;
}

void VulkanPartitionedTLAS::SetName(const std::string& name) {
    RHIResource::SetName(name);
#if MI_ENABLE_RHI_OBJECT_NAMING
    if (buffer_) {
        auto device = GetVulkanRHI()->GetDevice();
        vk::DebugUtilsObjectNameInfoEXT name_info{
            vk::ObjectType::eBuffer,
            reinterpret_cast<uint64_t>(static_cast<VkBuffer>(buffer_)),
            name.c_str()
        };
        device.setDebugUtilsObjectNameEXT(name_info);
    }
#endif
}

MI_NAMESPACE_END
