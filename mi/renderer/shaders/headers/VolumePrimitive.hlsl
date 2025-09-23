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

// 计算一个傅里叶分布在x处的密度
float GetRayVolumeDistributionDensity(RayVolumeDistribution Distribution, float x) {
    float Density = Distribution.Density_fourier_a[0] * 0.5f;

    float l = Distribution.l;
    float r = Distribution.r;
    float u = PI * (x - l) / max(r - l, 1e-6);
    for(uint i = 1; i <= Distribution.fourier_order; i++) {
        float n_u = i * u;
        Density += Distribution.Density_fourier_a[i] * cos(n_u) + Distribution.Density_fourier_b[i] * sin(n_u);
    }
    return Density;
}

// 计算一个傅里叶分布在x处的颜色
float3 GetRayVolumeDistributionColor(RayVolumeDistribution Distribution, float x) {
    float3 Color = Distribution.Color_fourier_a[0] * 0.5f;

    float l = Distribution.l;
    float r = Distribution.r;
    float u = PI * (x - l) / max(r - l, 1e-6);
    for(uint i = 1; i <= Distribution.fourier_order; i++) {
        float n_u = i * u;
        Color += Distribution.Color_fourier_a[i] * cos(n_u) + Distribution.Color_fourier_b[i] * sin(n_u);
    }
    return Color;
}

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
float IntegrateRayVolumeDistributionDensity(RayVolumeDistribution fourier_distr, float x) {
    float l = fourier_distr.l;
    float r = fourier_distr.r;

    float omega = PI / (r - l);

    float integral = (fourier_distr.Density_fourier_a[0]) * 0.5f * (x - l);
    for(int i = 1; i <= fourier_distr.fourier_order; i++) {
        float n_omega = i * omega;
        float n_omega_inv = 1.f / n_omega;
        float u = n_omega * (x - l);
        integral += fourier_distr.Density_fourier_a[i] * n_omega_inv * sin(u);
        integral += fourier_distr.Density_fourier_b[i] * n_omega_inv * (1.f - cos(u));
    }
    return integral;
}

// 对傅里叶级数体积分布进行自由程采样
float SampleRayVolumeDistribution(RayVolumeDistribution Distribution, float u) {
    // Sample free flight length from the distribution using inversion method
    float l = Distribution.l;
    float r = Distribution.r;

    //float FreeFlightLength = SampleExponentialScatteringMedium(Distribution.Density, u);
    // 二分法查找采样到的自由程
    float target = -log(1.f - u);
    float FreeFlightLength = 1e9f;

    uint max_iterations = 16;
    float tolerance = 1e-6;

    float low = l;
    float high = r;
    float mid;

    // 光线完全穿过体积的情况
    if(IntegrateRayVolumeDistributionDensity(Distribution, r) < target) {
        return 1e9f;
    }
    for(uint i = 0; i < max_iterations; i++) {
        mid = (low + high) * 0.5f;
        float F_mid = IntegrateRayVolumeDistributionDensity(Distribution, mid);
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

#endif // VOLUME_PRIMITIVE_HLSL