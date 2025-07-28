#ifndef HYBRID_TRACING_HLSL
#define HYBRID_TRACING_HLSL

#include "Packing.hlsl"

RWStructuredBuffer<uint> RWRayToTraceCount;

// Sometimes when involving ray compaction / continuation, allocate new rays on this buffer
RWStructuredBuffer<uint> RWRayToTraceListAllocator;
RWStructuredBuffer<uint> RWRayToTraceListBuffer;


RWStructuredBuffer<uint> RWRayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWRayToTraceStateBuffer;

// Optional (when the starting point is exactly on a pixel center)
RWStructuredBuffer<uint> RWRayToTraceOriginScreenCoordBuffer;

// Optional (when the ray origin is in world space)
RWStructuredBuffer<float3> RWRayToTraceOriginBuffer;

struct HybridTracingUB {
    float SSRT_RelativeTexelThickness;
    float RayContinuationBackwardBiasFactor;
    float DefaultTMax;
    uint Unused2;
};
ConstantBuffer<HybridTracingUB> HybridTracing_UB;

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

RayToTrace FetchRayToTraceWithWorldOrigin(uint RayIndex, float TMax) {
    RayToTrace RayToTrace = (RayToTrace)0;
    RayToTrace.Origin = RWRayToTraceOriginBuffer[RayIndex];
    RayToTrace.Direction = UnpackNormal(RWRayToTraceDirectionBuffer[RayIndex]);
    RayToTrace.OriginScreenCoord = RWRayToTraceOriginScreenCoordBuffer[RayIndex];
    uint RayToTraceState = RWRayToTraceStateBuffer[RayIndex];
    RayToTrace.TMax = TMax;
    RayToTrace.TCurrent = UnpackRayToTraceState(RayToTraceState, RayToTrace.bHit);
    return RayToTrace;
}

RayToTrace FetchRayToTraceWithScreenOrigin(uint RayIndex, float TMax) {
    RayToTrace RayToTrace = (RayToTrace)0;
    RayToTrace.OriginScreenCoord = RWRayToTraceOriginScreenCoordBuffer[RayIndex];
    RayToTrace.Direction = UnpackNormal(RWRayToTraceDirectionBuffer[RayIndex]);
    uint RayToTraceState = RWRayToTraceStateBuffer[RayIndex];
    RayToTrace.TMax = TMax;
    RayToTrace.TCurrent = UnpackRayToTraceState(RayToTraceState, RayToTrace.bHit);
    return RayToTrace;
}

#endif