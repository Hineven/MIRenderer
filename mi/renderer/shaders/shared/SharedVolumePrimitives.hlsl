#ifndef SHARED_VOLUME_PRIMITIVES_HLSL
#define SHARED_VOLUME_PRIMITIVES_HLSL

#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// 32 bytes per primitive
struct PackedVolumePrimitive {
    float3 Position;
    uint32_t PackedRotation_OpacityHi; // Quaternion rotation (xyz), snorm3x8 + opacity high 8 bits
    float3 Scales;
    uint32_t PackedColor_OpacityLo; // RGBA color, unorm 3x8 + opacity low 8 bits
};

struct VolumePrimitive {
    float3 Position;
    float4 Rotation;
    float3 Scales;
    float3 Color;
    float  Opacity; // Volume density, actually. Ranging from 0.0 to infinity
};

struct VolumePrimitivesHeader {
    uint32_t NumPrimitives; // Number of primitives in this volume primitives
    uint32_t PrimitiveOffset; // Offset in the volume primitives uber buffer where the primitives start.
};

MI_SHARED_HLSL_END

// TODO this is a fixed parameter for now.
#define VOLUME_PRIMITIVES_HENYEY_GREENSTEIN_PHASE_G 0

#endif