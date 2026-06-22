// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL
#define MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

#define RENDERABLE_VISIBLE_FLAG_BIT 0x1u
#define RENDERABLE_RAY_TRACED_FLAG_BIT 0x2u

// Default vertex format
struct RenderableHeader {
    // Metadata.w always store flags of a renderable
    float4 Metadata;
};

struct StaticMeshInstanceHeader {
    // Index of the static mesh which the instance refers to.
    uint StaticMeshIndex;
    uint Padding0;
    uint Padding1;
    uint Flags;
};

struct VolumeGridInstanceHeader {
    // Index of the volume primitives which the instance refers to.
    uint VolumeGridIndex;
    uint Padding0;
    uint Padding1;
    uint Flags;
};

struct GigaVoxelInstanceHeader {
    // Index of the GigaVoxel asset which the instance refers to.
    uint GigaVoxelIndex;
    uint Padding0;
    uint Padding1;
    uint Flags;
};

// InstanceCustomIndex contract is now PER-RENDERABLE-TYPE (no shared layout).
//
// Each ray-traced renderable type defines its own InstanceCustomIndex layout:
//   - StaticMesh / VolumeGrid: the full 24 bits are the plain RenderableIndex
//     (scene slot). Renderable type is determined by the SBT hit group, NOT by
//     any bits in InstanceCustomIndex. So InstanceID() == RenderableIndex.
//   - GigaVoxel: [GigaVoxelInstanceRTHeaderIndex:8 bits 16-23]
//               [chunk_header_index:16 bits 0-15].
//     See SharedGigaVoxel.hlsl (GigaVoxelInstanceRTHeader + decode helper).
//
// Renderable type is no longer encoded in InstanceCustomIndex. Hit shaders
// distinguish types via the SBT record (each type has its own anyhit/closesthit
// entry), and the path tracer records type-specific flags in its own functions
// per closesthit entry.
//
// MI_RENDERABLE_TYPE_* macros are injected by RDGShader at compile time.
// They map renderable class names to their runtime-assigned indices.
// Examples: MI_RENDERABLE_TYPE_StaticMesh=0, MI_RENDERABLE_TYPE_VolumeGrid=1, etc.

#ifdef MI_SHADER

StaticMeshInstanceHeader GetStaticMeshInstanceHeader(RenderableHeader Header) {
    StaticMeshInstanceHeader Result;
    Result.StaticMeshIndex = asuint(Header.Metadata.x);
    Result.Padding0 = asuint(Header.Metadata.y);
    Result.Padding1 = asuint(Header.Metadata.z);
    Result.Flags = asuint(Header.Metadata.w);
    return Result;
}

VolumeGridInstanceHeader GetVolumeGridInstanceHeader(RenderableHeader Header) {
    VolumeGridInstanceHeader Result;
    Result.VolumeGridIndex = asuint(Header.Metadata.x);
    Result.Padding0 = asuint(Header.Metadata.y);
    Result.Padding1 = asuint(Header.Metadata.z);
    Result.Flags = asuint(Header.Metadata.w);
    return Result;
}

GigaVoxelInstanceHeader GetGigaVoxelInstanceHeader(RenderableHeader Header) {
    GigaVoxelInstanceHeader Result;
    Result.GigaVoxelIndex = asuint(Header.Metadata.x);
    Result.Padding0 = asuint(Header.Metadata.y);
    Result.Padding1 = asuint(Header.Metadata.z);
    Result.Flags = asuint(Header.Metadata.w);
    return Result;
}

#endif // MI_SHADER

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL