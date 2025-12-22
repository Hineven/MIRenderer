#ifndef HYBRID_TRACING_HLSL
#define HYBRID_TRACING_HLSL

#include "Packing.hlsl"

uint PackRayToTraceState (float RayTCurrent, bool bHit) {
    // Use sign bit to encode hit
    return asuint(RayTCurrent) | (bHit ? 0x80000000u : 0u);
}

float UnpackRayToTraceState (uint RayToTraceState, out bool bHit) {
    bHit = (RayToTraceState & 0x80000000u) != 0u;
    return asfloat(RayToTraceState & 0x7fffffffu);
}

struct RayToTrace {
    // Valid when the ray origin is world space
    float3 Origin;
    // Valid when the ray origin is exactly on a pixel center
    uint2 OriginScreenCoord;
    float3 Direction;
    float TCurrent;
    float TMax;
    bool bHit;
};

#endif