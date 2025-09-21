#ifndef VOLUME_PRIMITIVE_HLSL
#define VOLUME_PRIMITIVE_HLSL

#include "Packing.hlsl"
#include "../shared/SharedVolumePrimitives.hlsl"
#include "VolumeScattering.hlsl"

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

// 描述光线与单个体素球的交
struct RayVolumePrimitiveIntersection {
    float l, r;
    float Density;
    float3 Color;
};

struct RayVolumeDistribution {
    float l, r;
    // Extinction coefficient. We assume that extinction coefficient equals
    // to the scattering coefficient
    // float Density;
    // float3 Color;
    // 采用傅里叶级数进行Density与Color的拟合
    uint fourier_order;
    float Density_fourier_a[8];
    float Density_fourier_b[8]; // b0=0
    float3 Color_fourier_a[8];
    float3 Color_fourier_b[8]; // b0=0
};

// 对单个体素球的交进行自由程采样
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

// 计算傅里叶级数到x点的积分
float integrate_fourier(RayVolumeDistribution fourier_distr, float x) {
    float l = fourier_distr.l;
    float r = fourier_distr.r;

    float u_x = PI * (x - l) / (r - l);
    float constant = 0.f;
    float integral = (fourier_distr.Density_fourier_a[0]) * (x - l);
    for(int i = 1; i <= fourier_distr.fourier_order; i++) {
        float n_inv = 1.f / i;
        integral += fourier_distr.Density_fourier_a[i] * n_inv * sin(i * u_x)
            - fourier_distr.Density_fourier_b[i] * n_inv * cos(i * u_x);
        constant += fourier_distr.Density_fourier_b[i] * n_inv;
    }
    integral += constant;
    return integral * (r - l) / PI;
}

// 对傅里叶级数体积分布进行自由程采样
float SampleRayVolumeDistribution(RayVolumeDistribution Distribution, float u) {
    // Sample free flight length from the distribution using inversion method
    float l = Distribution.l;
    float r = Distribution.r;

    //float FreeFlightLength = SampleExponentialScatteringMedium(Distribution.Density, u);
    // 二分法查找采样到的自由程
    float target = -log(u);
    float FreeFlightLength = 1e9f;

    uint max_iterations = 8;
    float tolerance = 1e-6;

    float low = l;
    float high = 2 * r - l;
    float mid;

    for(uint i = 0; i < max_iterations; i++) {
        mid = (low + high) * 0.5;
        float F_mid = integrate_fourier(Distribution, mid);
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
    FreeFlightLength = mid;

    float Sample = l + FreeFlightLength;
    if(Sample > r) {
        // Sampled is out of bounds, return a large value
        return 1e9f;
    }
    return Sample;
}

#endif // VOLUME_PRIMITIVE_HLSL