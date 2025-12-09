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

struct GaussianRadianceFieldInstanceHeader {
    uint FieldIndex;
    uint Padding0;
    uint Padding1;
    uint Flags;
};

// The instance custom index is a 20-bit index and a 4-bit flag field.
#define INSTANCE_CUSTOM_INDEX_INDEX_MASK 0x000FFFFFu
// Flags marking the kind of the instance. defaults to static mesh instance (0).
#define INSTANCE_CUSTOM_INDEX_FLAGS_MASK 0x00F00000u
// The flag indicates that the instance is a static mesh instance.
#define INSTANCE_CUSTOM_INDEX_FLAG_NONE 0x00000000u
// The flag indicates that the instance is a volume primitives instance.
#define INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_PRIMITIVES 0x00100000u
// The flag indicates that the instance is a 3D gaussian radiance field.
#define INSTANCE_CUSTOM_INDEX_FLAG_GAUSSIAN_RADIANCE_FIELD 0x00200000u

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

GaussianRadianceFieldInstanceHeader GetGaussianRadianceFieldInstanceHeader(RenderableHeader Header) {
    GaussianRadianceFieldInstanceHeader Result;
    Result.FieldIndex = asuint(Header.Metadata.x);
    Result.Padding0 = asuint(Header.Metadata.y);
    Result.Padding1 = asuint(Header.Metadata.z);
    Result.Flags = asuint(Header.Metadata.w);
    return Result;
}

#endif // MI_SHADER

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL