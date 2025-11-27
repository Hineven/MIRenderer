#ifndef GAUSSIAN_RADIANCE_FIELD_RESOURCES_HLSL
#define GAUSSIAN_RADIANCE_FIELD_RESOURCES_HLSL
#include "../shared/SharedGaussianRadianceField.hlsl"
#include "../headers/SphericalHarmonics.hlsl"
#include "../headers/Packing.hlsl"
StructuredBuffer<PackedGaussian3D> Gaussian3DBuffer;
Gaussian3D UnpackGaussian(PackedGaussian3D PackedG) {
    Gaussian3D G;
    G.Position = PackedG.Position;
    G.Scales   = PackedG.Scales;
    float3 Rotation_xyz = UnpackSnorm4x8(PackedG.PackedRotation_Opacity).rgb;
    G.Rotation = normalize(float4(Rotation_xyz, sqrt(max(0.0f, 1.0f - dot(Rotation_xyz, Rotation_xyz)))));
    G.Opacity  = UnpackUnorm4x8(PackedG.PackedRotation_Opacity).a;
    return G;
}
Gaussian3D FetchGaussian(uint GaussianIndex) {
    return UnpackGaussian(Gaussian3DBuffer[GaussianIndex]);
}

StructuredBuffer<float3> GaussianSHBuffer;
SH3Coefficents FetchGaussianSHCoefficients(uint GaussianIndex) {
    SH3Coefficents SH;
    // Degree 3: 16x3 coefficients
    [unroll]
    for(uint i = 0; i < 16; ++i) {
        SH.Coefficients[i] = GaussianSHBuffer[GaussianIndex * 16 + i];
    }
    return SH;
}
StructuredBuffer<GaussianRadianceFieldHeader> GaussianRadianceFieldHeaderBuffer;
#endif // GAUSSIAN_RADIANCE_FIELD_RESOURCES_HLSL