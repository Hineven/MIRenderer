/*
 * Created: 2026/6/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RHI_PTLAS_H
#define RHI_PTLAS_H

#include <rhi/rhi_resource.h>
#include <rhi/rhi_buffer.h>
#include <rhi/rhi_as_types.h>

MI_NAMESPACE_BEGIN

// ============================================================================
// Partitioned Top-Level Acceleration Structure (PTLAS)
// VK_NV_partitioned_acceleration_structure
//
// Unlike a regular TLAS (which is a Vulkan object with a VkAccelerationStructureKHR
// handle), a PTLAS has NO Vulkan object handle. It is just a device-addressable
// buffer whose contents are filled by vkCmdBuildPartitionedAccelerationStructuresNV.
// The "acceleration structure" identity is purely the device address of that buffer.
//
// PTLAS instances carry a partitionIndex, allowing per-partition rebuilds instead
// of full TLAS rebuilds. See task_ptlas_support.md for the full design.
// ============================================================================

// Op types for PTLAS build operations, mirroring VkPartitionedAccelerationStructureOpTypeNV.
enum class RHIPTLASOpType {
    kWriteInstance              = 0, // Add/replace an instance (full data, can change transform)
    kUpdateInstance             = 1, // Only update BLAS ref + SBT offset (no transform change)
    kWritePartitionTranslation  = 2, // Assign a translation vector to a partition
    kMax
};

// Instance flags for PTLAS, mirroring VkPartitionedAccelerationStructureInstanceFlagBitsNV.
enum class RHIPTLASInstanceFlagBits : uint32_t {
    kNone                    = 0,
    kDisableTriangleCulling  = 1u << 0,
    kFlipTriangleFacing      = 1u << 1,
    kForceOpaque             = 1u << 2,
    kForceNoOpaque           = 1u << 3,
    kEnableExplicitAABB      = 1u << 4, // Use the explicitAABB field instead of BLAS bounds
};
MAKE_FLAGS(RHIPTLASInstance)

// Special partition index: each global instance acts as its own partition,
// not occupying a slot in partitionCount.
constexpr uint32_t kPTLASPartitionIndexGlobal = ~0u;

// Capacity descriptor. This is both the input for GetBuildSizes and the
// "input" field of the build command. Capacity is fixed at creation time.
struct RHIPartitionedTLASInstancesInput {
    RHIAccelerationStructureBuildFlags flags = RHIAccelerationStructureBuildFlagBits::kPreferFastTrace;
    uint32_t instance_count                        = 0; // Total instance count (across all partitions)
    uint32_t max_instance_per_partition_count      = 0;
    uint32_t partition_count                       = 0; // Number of partitions (excluding the global partition)
    uint32_t max_instance_in_global_partition_count = 0;
};

// Build sizes returned by GetBuildSizes.
struct RHIPartitionedTLASBuildSizes {
    size_t acceleration_structure_size = 0; // Size of the backing buffer
    size_t build_scratch_size          = 0; // Scratch memory for build
    // Note: PTLAS update uses the same scratch as build (the NV API does not
    // report a separate update scratch size; it reuses buildScratchSize).
};

// Instance data for the WRITE_INSTANCE op.
// Mirrors VkPartitionedAccelerationStructureWriteInstanceDataNV.
// DO NOT copy an array of this struct directly to the device buffer.
// The NV API consumes it through a strided device address; use the RHI build
// command which handles upload and indirect command generation.
struct RHIPartitionedTLASWriteInstance {
    float    transform[12];   // 3x4 row-major transform matrix
    float    explicit_aabb[6];// World-space AABB (minX, minY, minZ, maxX, maxY, maxZ)
    uint32_t instance_id;     // Custom index for shader access (24-bit)
    uint32_t instance_mask;   // Visibility mask (8-bit)
    uint32_t instance_contribution_to_hit_group_index; // SBT offset
    RHIPTLASInstanceFlags instance_flags;
    uint32_t instance_index;  // Linear instance index within the PTLAS
    uint32_t partition_index; // Partition this instance belongs to (or kPTLASPartitionIndexGlobal)
    uint64_t acceleration_structure; // BLAS device address
};

// Instance data for the UPDATE_INSTANCE op (no transform, no partition change).
// Mirrors VkPartitionedAccelerationStructureUpdateInstanceDataNV.
struct RHIPartitionedTLASUpdateInstance {
    uint32_t instance_index;                       // Linear instance index within the PTLAS
    uint32_t instance_contribution_to_hit_group_index; // SBT offset
    uint64_t acceleration_structure;               // BLAS device address
};

// Partition translation data for the WRITE_PARTITION_TRANSLATION op.
// Mirrors VkPartitionedAccelerationStructureWritePartitionTranslationDataNV.
struct RHIPartitionedTLASPartitionTranslation {
    uint32_t partition_index;  // Partition to assign the translation to
    float    translation[3];   // Translation vector
};

// A single build operation, referencing already-uploaded device data.
// Multiple ops are batched into one vkCmdBuildPartitionedAccelerationStructuresNV call.
// The caller (renderer) is responsible for uploading each op's arg_data to a device
// buffer and providing its device address + stride here.
struct RHIPartitionedTLASBuildOp {
    RHIPTLASOpType op_type      = RHIPTLASOpType::kWriteInstance;
    uint32_t       arg_count    = 0;
    uint64_t       arg_data     = 0; // Device address of the uploaded op data array
    uint64_t       arg_stride   = 0; // Stride between elements (sizeof the corresponding struct)
};

class RHIPartitionedTLAS : public RHIResource {
public:
    RHIPartitionedTLAS() = default;
    ~RHIPartitionedTLAS() override = default;

    // Query the build sizes for the given capacity descriptor.
    // Internally calls vkGetPartitionedAccelerationStructuresBuildSizesNV.
    virtual RHIPartitionedTLASBuildSizes GetBuildSizes(
        const RHIPartitionedTLASInstancesInput& input) const = 0;

    // Allocate the backing buffer for the acceleration structure data.
    // size comes from GetBuildSizes().acceleration_structure_size.
    // config is the capacity descriptor used to query sizes; it is stored so
    // GetConfig() remains valid and the build command can verify compatibility.
    // Call again with a different size to re-allocate (old buffer is released).
    virtual bool Allocate(size_t size, const RHIPartitionedTLASInstancesInput& config) = 0;

    // The device address of the backing buffer. This is the identity of the
    // PTLAS — it is what gets written into a PTLAS descriptor for shader binding.
    virtual uint64_t GetDeviceAddress() const = 0;

    // Size of the allocated backing buffer (for barriers).
    virtual size_t GetSize() const = 0;

    // The capacity this PTLAS was configured with.
    virtual const RHIPartitionedTLASInstancesInput& GetConfig() const = 0;
};

MI_NAMESPACE_END

#endif //RHI_PTLAS_H
