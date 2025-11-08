#ifndef SAMPLING_HLSL
#define SAMPLING_HLSL

#include "MathConstants.hlsl"

float3 SampleHemisphereUniform (float2 u) {
    float2 SinCos;
    sincos(2.f * PI * u.x, SinCos.x, SinCos.y);
    float Z = u.y;
    float R = sqrt(max(1.f - Z * Z, 0.f));
    return float3(R * SinCos.y, R * SinCos.x, Z);
}

float SampleHemisphereUniformPdf ()
{
    return 1.f / TWO_PI;
}

float3 SampleSphereUniform (float2 u) {
    float2 SinCos;
    sincos(2.f * PI * u.x, SinCos.x, SinCos.y);
    float Z = 1.f - 2.f * u.y;
    float R = sqrt(max(1.f - Z * Z, 0.f));
    return float3(R * SinCos.y, R * SinCos.x, Z);
}

float SampleSphereUniformPdf ()
{
    return 1.f / FOUR_PI;
}

float CalculateHaltonNumber(in uint index, in uint base)
{
    float f      = 1.0f;
    float result = 0.0f;

    for (uint i = index; i > 0;)
    {
        f /= base;
        result = result + f * (i % base);
        i = uint(i / float(base));
    }

    return result;
}

float2 CalculateHaltonSequence(in uint index)
{
    // 256 samples per pixel
    return float2(CalculateHaltonNumber((index & 0xFFu) + 1, 2),
                  CalculateHaltonNumber((index & 0xFFu) + 1, 3));
}

#endif