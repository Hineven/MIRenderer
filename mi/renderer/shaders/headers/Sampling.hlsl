#ifndef SAMPLING_HLSL
#define SAMPLING_HLSL

#include "MathConstants.hlsl"

float3 UniformSampleHemisphere (float2 u) {
    float2 SinCos;
    sincos(2.f * PI * u.x, SinCos.x, SinCos.y);
    float Z = u.y;
    float R = sqrt(max(1.f - Z * Z, 0.f));
    return float3(R * SinCos.y, R * SinCos.x, Z);
}

float UniformSampleHemispherePdf ()
{
    return 1.f / TWO_PI;
}

#endif