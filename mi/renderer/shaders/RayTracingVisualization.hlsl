#include "headers/CommonSamplers.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"

RaytracingAccelerationStructure TLAS;
RWTexture2D<float4> RWDebugOutput;
TextureCube<float4> EnvironmentMap;
SamplerState LinearSampler;

struct RayPayload {
    float4 Color;
};

[shader("raygeneration")]
void RayTracingVisualizationRaygen() {
    uint2 RayIndex = DispatchRaysIndex().xy;
    uint2 DispatchSize = DispatchRaysDimensions().xy;

    RayDesc Ray = (RayDesc)0;
    {
        CameraParameters C = GetActiveCamera();
        Ray.Origin = C.Position;
        float2 UV = ((float2)RayIndex + 0.5f.xx) / (float2)DispatchSize;
        float2 NDC2 = UVToNDC2(UV);
        Ray.Direction = NDC2ToCameraDirectionUnnormalized(C, NDC2);
        Ray.TMin = C.NearPlane;
        Ray.TMax = C.FarPlane;
    }
    RayPayload Payload = (RayPayload)0;
    TraceRay(
        TLAS,
        RAY_FLAG_NONE,
        0xFF, // Ray mask
        0,    // SBT offset
        0,    // SBT stride
        0,    // Miss shader index
        Ray,
        Payload
    );
    RWDebugOutput[RayIndex] = Payload.Color;
}

[shader("miss")]
void RayTracingVisualizationMiss(inout RayPayload Payload: SV_RayPayload) {
    float3 RayDirection = WorldRayDirection();
    float3 EnvironmentColor = EnvironmentMap.SampleLevel(LinearSampler, RayDirection, 0).xyz;
    Payload.Color = float4(EnvironmentColor, 1.0f);
}

[shader("closesthit")]
void RayTracingVisualizationClosestHit(inout RayPayload Payload: SV_RayPayload,
                                       BuiltInTriangleIntersectionAttributes Attributes: SV_IntersectionAttributes) {
    uint Triangle = PrimitiveIndex();
    uint Geometry = GeometryIndex();
}