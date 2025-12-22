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
    uint  PointOffset; // Offset to the start of the gaussian points array
    // Whether the gaussian colors are stored in sRGB color space (default false)
    // Set to true if the source gaussian point cloud is optimized with sRGB colors.
    uint  SRGBColorSpace; 
    uint  Padding;
};
MI_SHARED_HLSL_END
#endif // SHARED_GAUSSIAN_RADIANCE_FIELD_HLSL

