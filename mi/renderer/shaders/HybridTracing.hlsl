

StructuredBuffer<uint> RaysToTraceCount;
StructuredBuffer<uint2> RaysToTrace;

struct RayToTrace {
    uint2 OriginScreenCoords;
    float TCurrent, TMax;
    bool bHit;
};

struct HybridTracingUB {
    float OriginNormalOffset;
    float DefaultTMax;
    uint2 Pading;
};
ConstantBuffer<HybridTracingUB> HybridTracing_UB;

Texture2D<float> HiZBuffer;

void 

