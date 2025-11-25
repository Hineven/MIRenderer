#ifndef SHARED_GAUSSIAN_RADIANCE_FIELD_HLSL
#define SHARED_GAUSSIAN_RADIANCE_FIELD_HLSL
#include "SharedCommon.hlsl"
MI_SHARED_HLSL_BEGIN
// Matches PackedGaussianRadiance in C++
struct PackedGaussianRadiancePoint {
    float3 Position; // xyz
    uint   PackedRotation_OpacityHi; // snorm xyz + opacity hi8
    float3 Scales; // principal axes
    uint   PackedColor_OpacityLo; // unorm rgb + opacity lo8
};
struct GaussianRadiancePoint {
    float3 Position;
    float4 Rotation;
    float3 Scales;
    float3 Radiance; // direct radiance
    float  Opacity; // optional density
};
struct GaussianRadianceFieldHeader {
    uint NumPoints;
    uint PointOffset;
};
MI_SHARED_HLSL_END
#endif // SHARED_GAUSSIAN_RADIANCE_FIELD_HLSL

