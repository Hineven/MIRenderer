
Texture2D __internal__BindlessIndicesBuffer_Texture[];
SamplerState LinearSampler;
RWTexture2D<float4> OutputTexture;
RaytracingAccelerationStructure TLAS;


struct RayPayload {
    float2 Barycentrics;
    uint CustomIndex;
};

[shader("raygeneration")]
void RaygenMain()
{
    uint2 PixelCoords = DispatchRaysIndex().xy;
    uint2 OutputSize = DispatchRaysDimensions().xy;

    float2 NDC2 = (float2(PixelCoords) + 0.5f) / float2(OutputSize) * 2.0f - 1.0f;
    float AspectRatio = (float)OutputSize.x / (float)OutputSize.y;

    float3 RayOrigin = float3(0, 0, 2);
    float3 CameraUp = float3(0, 1, 0);
    float3 CameraRight = float3(1, 0, 1);
    float3 CameraForward = float3(0, 0, -1);
    float3 RayDirection = normalize(CameraForward +
                                   NDC2.x * CameraRight * AspectRatio +
                                   NDC2.y * CameraUp);

    RayDesc RayDesc;
    RayDesc.Origin = RayOrigin;
    RayDesc.Direction = RayDirection;
    RayDesc.TMin = 0.001f;
    RayDesc.TMax = 10000.0f;

    RayPayload Payload;

    TraceRay(
        TLAS,
        RAY_FLAG_NONE,
        0xFF,
        0,
        0,
        0,
        RayDesc,
        Payload
    );
    if(Payload.Barycentrics.x < 0)
        OutputTexture[PixelCoords] = float4(0, 0, 0, 1);
    else OutputTexture[PixelCoords] = float4(__internal__BindlessIndicesBuffer_Texture[Payload.CustomIndex].SampleLevel(LinearSampler, Payload.Barycentrics, 0).xyz, 1.0f);
}

[shader("miss")]
void MissMain(inout RayPayload Payload : SV_RayPayload)
{
    Payload.Barycentrics = -1.xx;
}

[shader("closesthit")]
void ClosestHitMain(inout RayPayload Payload : SV_RayPayload,
                    BuiltInTriangleIntersectionAttributes HitBuiltinAttribs : SV_IntersectionAttributes)
{
    // 返回三角形的重心坐标
    Payload.Barycentrics = HitBuiltinAttribs.barycentrics;
    Payload.CustomIndex = InstanceID();
}