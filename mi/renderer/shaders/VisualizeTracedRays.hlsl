#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/VertexShaderInstanceIndex.hlsl"
#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Math.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"

struct VisualizeTracedRaysVSOut {
    float4 Position : SV_POSITION;
    float  RayT : TEXCOORD0;
#ifdef VISUALIZE_RAY_COLORS
    float3 RayColor : TEXCOORD1;
#endif
};

StructuredBuffer<float3> TracedRaysOriginBuffer;
StructuredBuffer<float3> TracedRaysDirectionBuffer;
StructuredBuffer<uint> TracedRaysStateBuffer;

#ifdef VISUALIZE_RAY_COLORS
StructuredBuffer<float3> TracedRaysColorBuffer;
#endif

VisualizeTracedRaysVSOut VisualizeTracedRaysVS (
    uint VertexIndex : SV_VertexID,
    VERTEX_SHADER_INSTANCE_INDEX_SV_PARAMS
) {
    uint InstanceIndex = VS_INSTANCE_INDEX;
    VisualizeTracedRaysVSOut Output = (VisualizeTracedRaysVSOut)0;
    float3 RayOrigin = TracedRaysOriginBuffer[InstanceIndex];
    float3 RayDirection = TracedRaysDirectionBuffer[InstanceIndex];
    uint RayState = TracedRaysStateBuffer[InstanceIndex];
    float RayT = asfloat(RayState & 0x7fffffffu);
    bool bHit = bool(RayState & 0x80000000u);
    if (VertexIndex == 0) {
        Output.Position = mul(View.Camera.WorldToNDC_ReversedZ, float4(RayOrigin, 1));
        Output.RayT = 0;
    } else {
        Output.Position = mul(View.Camera.WorldToNDC_ReversedZ, float4(RayOrigin + RayDirection * RayT, 1));
        Output.RayT = bHit ? RayT : 0;
    }
#ifdef VISUALIZE_RAY_COLORS
    Output.RayColor = TracedRaysColorBuffer[InstanceIndex];
#endif
    return Output;
}

struct VisualizeTracedRaysPSOut {
    float4 Color : SV_TARGET0;
};

VisualizeTracedRaysPSOut VisualizeTracedRaysPS (
    VisualizeTracedRaysVSOut Input
) {
    VisualizeTracedRaysPSOut Output = (VisualizeTracedRaysPSOut)0;
#ifdef VISUALIZE_RAY_COLORS
    Output.Color = float4(Input.RayColor, 1);
#else
    Output.Color = float4(0, 0.5, 0.5, 1);//Input.RayT, Input.RayT, Input.RayT, 1);
#endif
    return Output;
}