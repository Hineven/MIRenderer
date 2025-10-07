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

#endif