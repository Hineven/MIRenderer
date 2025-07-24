#ifndef SHARED_VOLUME_PRIMITIVES_HLSL
#define SHARED_VOLUME_PRIMITIVES_HLSL

#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// 32 bytes per primitive
struct PackedVolumePrimitive {
    float3 Position;
    uint32_t PackedRotation; // Quaternion rotation, snorm4x8
    float3 Scales;
    uint32_t PackedColorOpacity; // RGBA color, packed into uint32_t
};

struct VolumePrimitive {
    float3 Position;
    float4 Rotation;
    float3 Scales;
    float3 Color;
    float  Opacity;
};

struct VolumePrimitivesHeader {
    uint32_t NumPrimitives; // Number of primitives in this volume primitives
    uint32_t PrimitiveOffset; // Offset in the volume primitives uber buffer where the primitives start.
};

MI_SHARED_HLSL_END

#endif