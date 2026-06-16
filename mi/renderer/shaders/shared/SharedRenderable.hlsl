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

struct VolumePrimitivesInstanceHeader {
    // Index of the volume primitives which the instance refers to.
    uint VolumePrimitivesIndex;
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

struct GaussianRadianceFieldInstanceHeader {
    uint FieldIndex;
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

// Number of bits for the renderable index in InstanceCustomIndex.
// Must match Renderable::kRenderableIndexNumBits on the C++ side.
#define RENDERABLE_INDEX_NUM_BITS 20

// The instance custom index layout: [class_index:4bits][renderable_index:RENDERABLE_INDEX_NUM_BITS bits]
#define INSTANCE_CUSTOM_INDEX_INDEX_MASK  ((1u << RENDERABLE_INDEX_NUM_BITS) - 1u)
#define INSTANCE_CUSTOM_INDEX_CLASS_SHIFT RENDERABLE_INDEX_NUM_BITS
#define INSTANCE_CUSTOM_INDEX_CLASS_MASK  (0xFu << RENDERABLE_INDEX_NUM_BITS)

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

VolumePrimitivesInstanceHeader GetVolumePrimitivesInstanceHeader(RenderableHeader Header) {
    VolumePrimitivesInstanceHeader Result;
    Result.VolumePrimitivesIndex = asuint(Header.Metadata.x);
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

GaussianRadianceFieldInstanceHeader GetGaussianRadianceFieldInstanceHeader(RenderableHeader Header) {
    GaussianRadianceFieldInstanceHeader Result;
    Result.FieldIndex = asuint(Header.Metadata.x);
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