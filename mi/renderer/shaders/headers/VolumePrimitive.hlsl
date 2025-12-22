#ifndef VOLUME_PRIMITIVE_HLSL
#define VOLUME_PRIMITIVE_HLSL

#include "Packing.hlsl"
#include "../shared/SharedVolumePrimitives.hlsl"
#include "VolumeScattering.hlsl"
#include "Fourier.hlsl"

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

// Ray intersect with one primitive
struct RayVolumePrimitiveIntersection {
    float l, r;
    float Density;
    float3 Color;
};

// Represent a segment of volume on the ray with uniform distribution
struct RayUniformVolumeDistribution {
    float l, r;
    float Density;
    float3 Color;
};

// Represent a segment of volume on the ray with quadratics
// (3 / 4) * Density * (1 - ((x - Mean) / Scale)^2) from [Mean - Scale, Mean + Scale]
struct RayQuadraticVolumeDistribution {
    float Mean, Scale;
    float Density;
    float3 Color;
};

// Fourier volume distribution
struct RayFourierVolumeDistribution {
    FourierFloat Density;
    FourierFloat3 WeightedColor; // Actually restores Density * Color
};

// Calculate the density of a Fourier distribution at x
float GetRayFourierVolumeDistributionDensity(RayFourierVolumeDistribution Distr, float x) {
    float Density = ComputeFourierValue(Distr.Density, x);

    return Density;
}

// Calculate the color of a Fourier distribution at x
float3 GetRayFourierVolumeDistributionColor(RayFourierVolumeDistribution Distr, float x) {
    float3 WeightedColor = ComputeFourierValue(Distr.WeightedColor, x);
    float Density = ComputeFourierValue(Distr.Density, x);

    float3 Color = float3(0.f, 0.f, 0.f);
    if(Density > 1e-6f) {
        Color = clamp((WeightedColor / Density), 0.f, 1.f);
    }
    return Color;
}

// Sample single volume primitive intersection ray distance
float SampleRayVolumePrimitiveIntersection(RayVolumePrimitiveIntersection Distribution, float u) {
    float l = Distribution.l;
    float r = Distribution.r;

    float FreeFlightLength = SampleExponentialScatteringMedium(Distribution.Density, u);

    float Sample = l + FreeFlightLength;
    if(Sample > r) {
        // Sampled is out of bounds, return a large value
        return 1e9f;
    }
    return Sample;
}

// Calculate the density integral of Fourier order from l to point x
float IntegrateRayFourierVolumeDistributionDensity(RayFourierVolumeDistribution Distr, float x) {
    float integral = ComputeFourierIntegral(Distr.Density, x);

    return integral;
}

// Free path sampling of Fourier volume distribution
float SampleRayFourierVolumeDistribution(RayFourierVolumeDistribution Distr, float u) {
    // Sample free flight length from the distribution using inversion method
    float l = Distr.Density.l;
    float r = Distr.Density.r;

    // calculate the target -log(transmittance) = integral of Density
    float target = -log(1.f - u);
    float FreeFlightLength = 1e9f;

    uint max_iterations = 16;
    float tolerance = 1e-6;

    float low = l;
    float high = r;
    float mid;

    // the ray cross the whole volume
    if(IntegrateRayFourierVolumeDistributionDensity(Distr, r) < target) {
        return 1e9f;
    }

    for(uint i = 0; i < max_iterations; i++) {
        mid = (low + high) * 0.5f;
        float F_mid = IntegrateRayFourierVolumeDistributionDensity(Distr, mid);
        if(abs(F_mid - target) < tolerance) {
            FreeFlightLength = mid;
            break;
        }
        if(F_mid < target) {
            low = mid;
        }
        else {
            high = mid;
        }
    }
    FreeFlightLength = (low + high) * 0.5f;

    return FreeFlightLength;
}

// Return the decrease in extinction coefficient when traversing through volume primitives
// NormalizedCenterDistance is the minimum nomalized distance from the ray line to
// the center of the volume primitive. Ranging from 0 to 1.
float VolumePrimitiveRayDecay (float NormalizedCenterDistance) {
    float u = NormalizedCenterDistance;
    // Simple quadratic falloff
    return (1 - u * u);
}

#endif // VOLUME_PRIMITIVE_HLSL