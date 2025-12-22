#ifndef MATH_HLSL
#define MATH_HLSL

#include "MathConstants.hlsl"
#include "Select.hlsl"

float Squared (float x) {
    return x * x;
}

// Clamp to [0, 1)
float saturateDown (float Value) {
    return clamp(Value, 0.0f, 1.f - FLT_EPSILON);
}
float2 saturateDown (float2 Value) {
    return clamp(Value, 0.0f.xx, 1.f.xx - FLT_EPSILON.xx);
}
float3 saturateDown (float3 Value) {
    return clamp(Value, 0.0f.xxx, 1.f.xxx - FLT_EPSILON.xxx);
}
float4 saturateDown (float4 Value) {
    return clamp(Value, 0.0f.xxxx, 1.f.xxxx - FLT_EPSILON.xxxx);
}

// Clamp to (0, 1]
float saturateUp (float Value) {
    return clamp(Value, FLT_EPSILON, 1.0f);
}
float2 saturateUp (float2 Value) {
    return clamp(Value, FLT_EPSILON, 1.0f);
}

float  InterpolateBarycentrics (float A, float B, float C, float2 UV) {
    return A * (1 - UV.x - UV.y) + B * UV.x + C * UV.y;
}
float2 InterpolateBarycentrics(float2 A, float2 B, float2 C, float2 UV) {
    return A * (1 - UV.x - UV.y) + B * UV.x + C * UV.y;
}
float3 InterpolateBarycentrics (float3 A, float3 B, float3 C, float2 UV) {
    return A * (1 - UV.x - UV.y) + B * UV.x + C * UV.y;
}

uint hadd (uint2 Value) {
    return Value.x + Value.y;
}

uint hadd (uint3 Value) {
    return Value.x + Value.y + Value.z;
}

uint hadd (uint4 Value) {
    return Value.x + Value.y + Value.z + Value.w;
}

float hmin (float2 Value) {
    return min(Value.x, Value.y);
}

float hmin (float3 Value) {
    return min(min(Value.x, Value.y), Value.z);
}

float hmin (float4 Value) {
    return min(min(min(Value.x, Value.y), Value.z), Value.w);
}

float hmax (float2 Value) {
    return max(Value.x, Value.y);
}

float hmax (float3 Value) {
    return max(max(Value.x, Value.y), Value.z);
}

float hmax (float4 Value) {
    return max(max(max(Value.x, Value.y), Value.z), Value.w);
}

/*
 Based on John D. Vedder, "Simple approximations for the error function and its
 inverse." American Journal of Physics, Vol. 55, No. 8, Aug. 1987, pp. 762-763.

 maximum ulp error: 5735.81, maximum relative error: 3.8735e-4
*/
float erf_approax_fast (float x)
{
    float x2 = x * x;
    x = ((0.100646973f * x2 + 0.128759325f) * x + x); // 0x1.9c4000p-4, 0x1.07b2f8p-3
    return tanh(x);
}

// Fast approximate error function (Abramowitz & Stegun 7.1.26)
// Max abs error ~1.5e-7 for float;
float erf_approx(float x)
{
    const float a1 = 0.254829592f;
    const float a2 = -0.284496736f;
    const float a3 = 1.421413741f;
    const float a4 = -1.453152027f;
    const float a5 = 1.061405429f;
    const float p  = 0.3275911f;
    float sgn = x < 0 ? -1.0f : 1.0f;
    x = abs(x);
    float t = 1.0f / (1.0f + p * x);
    float y = 1.0f - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t * exp(-x * x);
    return sgn * y;
}

float3 erf_approx(float3 v)
{
    return float3(erf_approx(v.x), erf_approx(v.y), erf_approx(v.z));
}

#endif