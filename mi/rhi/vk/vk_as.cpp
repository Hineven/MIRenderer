/*
 * Created: 2025/1/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "vk_as.h"
#include "vk_conversion.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

VulkanAccelerationStructure::VulkanAccelerationStructure(RHIAccelerationStructureType type)
    : RHIAccelerationStructure(type), allocation_(nullptr) {
}

VulkanAccelerationStructure::~VulkanAccelerationStructure() {
    ResetRHI();
}

size_t VulkanAccelerationStructure::GetSize() const {
    return size_;
}

bool VulkanAccelerationStructure::Create(size_t size) {
    if (size == 0) {
        MI_LOG(MIInfraLogType::kWarning, "Acceleration structure size cannot be zero");
        return false;
    }
    ResetRHI();

    auto device = GetVulkanRHI()->GetDevice();
    auto& allocator = GetVulkanRHI()->GetVmaAllocator();

    vk::BufferCreateInfo buffer_info{};
    buffer_info.size = size;
    buffer_info.usage = vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR |
                        vk::BufferUsageFlagBits::eShaderDeviceAddress;
    buffer_info.sharingMode = vk::SharingMode::eExclusive;

    vma::AllocationCreateInfo alloc_info{};
    alloc_info.usage = vma::MemoryUsage::eAutoPreferDevice;
    alloc_info.flags = vma::AllocationCreateFlagBits::eDedicatedMemory;
    alloc_info.requiredFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;

    auto [buffer_result, allocation_result] = allocator.createBuffer(buffer_info, alloc_info);

    buffer_ = buffer_result;
    allocation_ = allocation_result;

    vk::AccelerationStructureCreateInfoKHR create_info{};
    create_info.buffer = buffer_;
    create_info.size = size;
    create_info.type = (type_ == RHIAccelerationStructureType::kBottomLevel)
        ? vk::AccelerationStructureTypeKHR::eBottomLevel
        : vk::AccelerationStructureTypeKHR::eTopLevel;

    auto as_result = device.createAccelerationStructureKHR(create_info);
    acceleration_structure_ = as_result;
    size_ = size;
    return true;
}

uint64_t VulkanAccelerationStructure::GetDeviceAddress() const {
    if (!acceleration_structure_) {
        return 0;
    }

    vk::AccelerationStructureDeviceAddressInfoKHR address_info{};
    address_info.accelerationStructure = acceleration_structure_;

    return GetVulkanRHI()->GetDevice().getAccelerationStructureAddressKHR(address_info);
}

RHIAccelerationStructureBuildSizesInfo VulkanAccelerationStructure::GetBuildSizes(
    const RHIAccelerationStructureBuildGeometryInfo& build_info) const {

    RHIAccelerationStructureBuildSizesInfo sizes{};

    // Convert RHI build info to Vulkan format
    vk::AccelerationStructureBuildTypeKHR build_type = vk::AccelerationStructureBuildTypeKHR::eDevice;

    std::vector<vk::AccelerationStructureGeometryKHR> geometries;
    std::vector<uint32_t> primitive_counts;

    if (build_info.type == RHIAccelerationStructureType::kBottomLevel) {
        // Convert geometries for BLAS
        for (const auto& geom : build_info.geometries) {
            vk::AccelerationStructureGeometryKHR vk_geom{};
            vk_geom.flags = GetVulkanGeometryFlags(geom.flags);

            if (geom.type == RHIASGeometryType::kTriangles) {
                vk_geom.geometryType = vk::GeometryTypeKHR::eTriangles;
                vk_geom.geometry.triangles.vertexData =
                    ((VulkanBuffer*)geom.triangles.vertex_data.buffer)->GetDeviceAddress() + geom.triangles.vertex_data.offset;
                vk_geom.geometry.triangles.vertexStride = geom.triangles.vertex_stride;
                vk_geom.geometry.triangles.vertexFormat = vk::Format::eR32G32B32Sfloat; // Only support float3
                vk_geom.geometry.triangles.maxVertex = geom.triangles.vertex_count;
                auto index_address =
                    ((VulkanBuffer*)geom.triangles.index_data.buffer)->GetDeviceAddress()
                    + geom.triangles.index_data.offset;
                if (geom.triangles.index_data.IsValid()) {
                    vk_geom.geometry.triangles.indexData = vk::DeviceOrHostAddressConstKHR{index_address};
                    vk_geom.geometry.triangles.indexType = vk::IndexType::eUint32; // Only support uint32 indices
                }

                primitive_counts.push_back(geom.triangles.index_count > 0 ?
                    geom.triangles.index_count / 3 : geom.triangles.vertex_count / 3);
            } else if (geom.type == RHIASGeometryType::kAABBs) {
                vk_geom.geometryType = vk::GeometryTypeKHR::eAabbs;
                vk_geom.geometry.aabbs.data =
                    ((VulkanBuffer*)geom.aabbs.aabb_data.buffer)->GetDeviceAddress() + geom.aabbs.aabb_data.offset;
                vk_geom.geometry.aabbs.stride = geom.aabbs.aabb_stride;

                primitive_counts.push_back(geom.aabbs.aabb_count);
            }

            geometries.push_back(vk_geom);
        }
    } else {
        // For TLAS, we have instances
        assert(build_info.geometries.size() == 0 && "TLAS should not have geometries, use instance data instead");
        primitive_counts.push_back(build_info.instance_count);
        vk::AccelerationStructureGeometryKHR vk_geom{};
        vk_geom.geometry.instances = vk::AccelerationStructureGeometryInstancesDataKHR{};
        vk_geom.geometryType = vk::GeometryTypeKHR::eInstances;
        // TODO support opaque TLAS ?
        vk_geom.flags = GetVulkanGeometryFlags(RHIASGeometryFlagBits::kNone);
        geometries.push_back(vk_geom);
    }

    // Query build sizes
    vk::AccelerationStructureBuildGeometryInfoKHR vk_build_info{};
    vk_build_info.type = (build_info.type == RHIAccelerationStructureType::kBottomLevel)
        ? vk::AccelerationStructureTypeKHR::eBottomLevel
        : vk::AccelerationStructureTypeKHR::eTopLevel;
    vk_build_info.flags = GetVulkanBuildAccelerationStructureFlags(build_info.flags);
    vk_build_info.mode = (build_info.mode == RHIAccelerationStructureBuildMode::kBuild)
        ? vk::BuildAccelerationStructureModeKHR::eBuild
        : vk::BuildAccelerationStructureModeKHR::eUpdate;
    vk_build_info.geometryCount = static_cast<uint32_t>(geometries.size());
    vk_build_info.pGeometries = geometries.data();

    vk::AccelerationStructureBuildSizesInfoKHR vk_sizes = GetVulkanRHI()->GetDevice()
        .getAccelerationStructureBuildSizesKHR(build_type, vk_build_info, primitive_counts);

    sizes.acceleration_structure_size = vk_sizes.accelerationStructureSize;
    sizes.build_scratch_size = vk_sizes.buildScratchSize;
    sizes.update_scratch_size = vk_sizes.updateScratchSize;

    return sizes;
}

void VulkanAccelerationStructure::ResetRHI() {
    auto device = GetVulkanRHI()->GetDevice();
    auto& allocator = GetVulkanRHI()->GetVmaAllocator();

    if (acceleration_structure_) {
        device.destroyAccelerationStructureKHR(acceleration_structure_);
        acceleration_structure_ = nullptr;
    }

    if (buffer_ && allocation_) {
        allocator.destroyBuffer(buffer_, allocation_);
        buffer_ = nullptr;
        allocation_ = nullptr;
    }

    size_ = 0;
}

void VulkanAccelerationStructure::SetName(const std::string& name) {
    RHIResource::SetName(name);
#ifndef NDEBUG
    auto device = GetVulkanRHI()->GetDevice();
    vk::DebugUtilsObjectNameInfoEXT name_info{
        vk::ObjectType::eAccelerationStructureKHR,
        reinterpret_cast<uint64_t>(static_cast<VkAccelerationStructureKHR>(acceleration_structure_)),
        name.c_str()
    };
    device.setDebugUtilsObjectNameEXT(name_info);
#endif
}

MI_NAMESPACE_END
