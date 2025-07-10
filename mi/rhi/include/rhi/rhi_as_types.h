/*
 * Created: 2025/7/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RHI_AS_TYPES_H
#define RHI_AS_TYPES_H
#include <vector>
#include "core/common.h"

MI_NAMESPACE_BEGIN


enum class RHIAccelerationStructureType {
    kBottomLevel,   // BLAS - Bottom Level Acceleration Structure
    kTopLevel,      // TLAS - Top Level Acceleration Structure
    kMax
};

enum class RHIAccelerationStructureBuildFlagBits : uint32_t {
    kNone = 0,
    kAllowUpdate = 1u << 0,           // Allow updates to the acceleration structure
    kAllowCompaction = 1u << 1,       // Allow compaction of the acceleration structure
    kPreferFastTrace = 1u << 2,       // Optimize for trace performance
    kPreferFastBuild = 1u << 3,       // Optimize for build performance
    kLowMemory = 1u << 4,             // Minimize memory usage during build
};
MAKE_FLAGS(RHIAccelerationStructureBuild)

enum class RHIAccelerationStructureBuildMode {
    kBuild,         // Build from scratch
    kUpdate,        // Update existing acceleration structure
    kMax
};

enum class RHIASGeometryType {
    kTriangles,     // Triangle geometry
    kAABBs,         // Axis-aligned bounding boxes
    kMax
};

enum class RHIASGeometryFlagBits : uint32_t {
    kNone = 0,
    kOpaque = 1u << 0,                    // Geometry is opaque (no any-hit shader)
    kNoDuplicateAnyHitInvocation = 1u << 1, // No duplicate any-hit invocations
};
MAKE_FLAGS(RHIASGeometry)

// Geometry description for BLAS
struct RHIASGeometryTriangles {
    RHIBufferSpan vertex_data;          // Vertex buffer
    uint32_t vertex_stride;             // Stride between vertices (bytes)
    uint32_t vertex_count;              // Number of vertices
    RHIVertexAttributeFormatType vertex_format; // Vertex position format, must be float3

    RHIBufferSpan index_data;           // Index buffer (optional)
    uint32_t index_count;               // Number of indices (or 0 if no indices)
    RHIIndexType index_type;            // Index format

    RHIBufferSpan transform_data;       // Transform matrix (optional, 3x4 matrix)
};

struct RHIASGeometryAABBs {
    RHIBufferSpan aabb_data;            // AABB buffer (6 floats per AABB: minX, minY, minZ, maxX, maxY, maxZ)
    uint32_t aabb_count;                // Number of AABBs
    uint32_t aabb_stride;               // Stride between AABBs
};

struct RHIASGeometry {
    RHIASGeometryType type;
    RHIASGeometryFlags flags;

    union {
        RHIASGeometryTriangles triangles;
        RHIASGeometryAABBs aabbs;
    } ;
};

// Instance description for TLAS
// DO NOT copy an array of this struct directly to the instance buffer when building a TLAS.
// Use RHI::CreateAccelerationStructureInstance(RHIAccelerationStructureInstanceDesc, void*) to convert them before uploading.
struct RHIAccelerationStructureInstanceDesc {
    float transform[12];                // 3x4 transform matrix (row-major)
    uint32_t instance_custom_index : 24;// Custom index for shader access
    uint32_t mask : 8;                  // Visibility mask
    uint32_t instance_shader_binding_table_record_offset : 24; // SBT offset
    uint32_t flags : 8;                 // Instance flags
    uint64_t acceleration_structure_reference; // Reference to BLAS
};

// Build information for acceleration structures
struct RHIAccelerationStructureBuildGeometryInfo {
    RHIAccelerationStructureType type;
    RHIAccelerationStructureBuildFlags flags;
    RHIAccelerationStructureBuildMode mode;

    RHIAccelerationStructure* src_acceleration_structure; // Source AS for updates
    RHIAccelerationStructure* dst_acceleration_structure; // Destination AS

    // For BLAS
    std::span<RHIASGeometry> geometries;

    // For TLAS
    RHIBufferSpan instance_data;        // Instance buffer for TLAS
    uint32_t instance_count;            // Number of instances
};

struct RHIAccelerationStructureBuildSizesInfo {
    size_t acceleration_structure_size; // Size of the acceleration structure
    size_t update_scratch_size;         // Scratch memory size for updates
    size_t build_scratch_size;          // Scratch memory size for builds
};

MI_NAMESPACE_END
#endif //RHI_AS_TYPES_H
