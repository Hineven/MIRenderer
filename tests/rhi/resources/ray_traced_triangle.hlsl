
Texture2D __internal__BindlessIndicesBuffer_Texture[];
SamplerState LinearSampler;
RWTexture2D<float4> OutputTexture;
AccelerationStructure TLAS;

[shader("raygeneration")]
void RayGenMain()
{
    uint2 PixelCoords = DispatchRaysIndex().xy;
    uint2 OutputSize = DispatchRaysDimensions().xy;

    float2 NDC2 = (float2(PixelCoords) + 0.5f) / float2(OutputSize) * 2.0f - 1.0f;
    float AspectRatio = (float)OutputSize.x / (float)OutputSize.y;

    float3 RayOrigin = float3(0, 0, 3)
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

    float4 result = TraceRay(
        TLAS,                 // 顶级加速结构
        RAY_FLAG_NONE,        // 射线标志
        0xFF,                 // 实例遮罩
        0,                    // 命中组索引
        0,                    // 命中组索引偏移
        0,                    // 命中组索引掩码
        RayDesc,              // 射线描述
        0                     // 输出数据的寄存器索引
    );
    if(result.x < 0)
        OutputTexture[PixelCoords] = float4(0, 0, 0, 1);
    else OutputTexture[PixelCoords] = float4(__internal__BindlessIndicesBuffer_Texture[0].SampleLevel(LinearSampler, result.xyz).xyz, 1.0f);
}

struct RayPayload {
    float3 Barycentrics;
};

// 未命中着色器
[shader("miss")]
void MissMain(inout RayPayload Payload : SV_RayPayload)
{
    // 未命中时返回-1
    Payload.Barycentrics = float3(-1);
}

// 最近命中着色器
[shader("closesthit")]
void ClosestHitMain(inout RayPayload Payload : SV_RayPayload,
                    BuiltInTriangleIntersectionAttributes HitBuiltinAttribs : SV_IntersectionAttributes)
{
    // 返回三角形的重心坐标
    Payload.Barycentrics = HitBuiltinAttribs.Barycentrics;
}