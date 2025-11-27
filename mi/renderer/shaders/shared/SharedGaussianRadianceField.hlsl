#ifndef SHARED_GAUSSIAN_RADIANCE_FIELD_HLSL
#define SHARED_GAUSSIAN_RADIANCE_FIELD_HLSL
#include "SharedCommon.hlsl"
MI_SHARED_HLSL_BEGIN
// Matches PackedGaussian3D in C++
struct PackedGaussian3D {
    float3 Position; // World-space center
    uint   PackedRotation_Opacity; // xyz quaternion (snorm3x8) + unorm1x8
    float3 Scales; // Principal axes scaling (pre-exponential activation applied)
};
struct Gaussian3D {
    float3 Position;
    float4 Rotation;
    float3 Scales;
    float  Opacity; // optional density
};
struct GaussianRadianceFieldHeader {
    uint  NumPoints;
    uint  PointOffset;
    uint2 Padding;
};
MI_SHARED_HLSL_END
#endif // SHARED_GAUSSIAN_RADIANCE_FIELD_HLSL

