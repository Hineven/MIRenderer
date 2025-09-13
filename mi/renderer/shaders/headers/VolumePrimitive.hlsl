#ifndef VOLUME_PRIMITIVE_HLSL
#define VOLUME_PRIMITIVE_HLSL

#include "Packing.hlsl"
#include "../shared/SharedVolumePrimitives.hlsl"

VolumePrimitive UnpackVolumePrimitive(PackedVolumePrimitive PackedPrimitive) {
    VolumePrimitive Primitive;
    Primitive.Position = PackedPrimitive.Position;
    float3 Rotation_xyz = UnpackSnorm4x8(PackedPrimitive.PackedRotation_OpacityHi).xyz;
    float Rotation_w = sqrt(max(1.0f - dot(Rotation_xyz, Rotation_xyz), 0.0f));
    float4 Rotation = float4(Rotation_xyz, Rotation_w);
    Primitive.Rotation = Rotation;
    Primitive.Scales = PackedPrimitive.Scales;
    float3 Color = UnpackUnorm4x8(PackedPrimitive.PackedColor_OpacityLo).rgb;
    Primitive.Color = Color;
    float Opacity = f16tof32(
        ((PackedPrimitive.PackedRotation_OpacityHi >> 24) << 8) |
        (PackedPrimitive.PackedColor_OpacityLo >> 24)
    );
    Primitive.Opacity = Opacity;
    return Primitive;
}

struct RayVolumeDistribution {
    float l, r;
    // Extinction coefficient. We assume that extinction coefficient equals
    // to the scattering coefficient
    float Density;
    float3 Color;
};

// Return the sampled ray distance
float SampleRayVolumeDistribution(RayVolumeDistribution Distribution, float u) {
    // Sample free flight length from the distribution using inversion method
    float l = Distribution.l;
    float r = Distribution.r;
    float Density = Distribution.Density;
    float FreeFlightLength = - log(1 - u) / max(Density, 1e-6f);
    float Sample = l + FreeFlightLength;
    if(Sample > r) {
        // Sampled is out of bounds, return a large value
        return 1e9f;
    }
    return Sample;
}

#endif // VOLUME_PRIMITIVE_HLSL