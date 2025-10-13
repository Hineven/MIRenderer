// AmbientOcclusion.hlsl
// ------------------------------------------------------------
// Screen-Space Horizon Based Ambient Occlusion (HBAO) + 2-pass
// separable bilateral blur (horizontal & vertical) to produce
// a stable AO texture. Written to follow existing project style.
// ------------------------------------------------------------
// 设计目标:
// 1. 只依赖深度 + 法线 (GBuffer) + 随机旋转噪声 (可选)
// 2. 单一 HBAO 原始 pass 生成 Raw AO (未模糊)
// 3. 双边模糊横向 -> 纵向，两次 compute，利用深度与法线保持边界
// 4. 采用视空间半径与步进分离，方向数与步数可调
// 5. 线程组使用 8x8，便于在多数 GPU 上达到较好 occupancy
// 6. 简化: 不生成 bent normal，仅 AO 标量；如需扩展可以再加 RW 目标
// ------------------------------------------------------------

// 依赖: 需要 CameraParameters 及相关函数。若工程内已有集中包含，可改为对应路径。
// 尝试包含常用相机与共享视图头；若已有外层统一 include，可移除下面两行避免重复。
#ifndef AO_INCLUDED_CAMERA_HEADERS
#define AO_INCLUDED_CAMERA_HEADERS 1
#include "headers/Camera.hlsl"
#endif

#ifndef HBAO_WAVE_SIZE
#define HBAO_WAVE_SIZE 32
#endif

#ifndef HBAO_THREAD_GROUP_SIZE_X
#define HBAO_THREAD_GROUP_SIZE_X 8
#endif
#ifndef HBAO_THREAD_GROUP_SIZE_Y
#define HBAO_THREAD_GROUP_SIZE_Y 8
#endif

// 常量参数（可在 CPU 侧填入）
cbuffer HBAOParameters
{
    float  AO_Radius;                 // 视空间采样半径 (meters)
    float  AO_Bias;                   // 减少自遮挡 (meters)
    float  AO_Power;                  // 对比度增强曲线 (典型 1.0~2.0)
    uint   AO_NumDirections;          // 方向数量 (例如 8 / 12 / 16)
    uint   AO_StepsPerDirection;      // 每个方向步数 (例如 4~8)
    float  AO_StepDistribution;       // 步进分布 (0=线性, 1=平方), 介于[0,1]
    float  AO_Strength;               // 强度缩放
    float  AO_FalloffDistance;        // 超出该视空间距离衰减 (>= Radius)
    float  AO_MaxScreenRadius;        // 屏幕空间最大像素半径 (防止过大过滤)
    float  AO_NormalPower;            // 法线相似度权重指数 (1~4)  // (保留, 预留未来按法线方向分布权重)
    float  AO_DepthSigma;             // 双边模糊深度 sigma (视空间)
    float  AO_NormalSigma;            // 双边模糊法线 sigma (角度差)
    float  AO_BlurEdgeSharpness;      // 边缘锐度 (0~1)
    uint   AO_FrameIndex;             // 用于旋转噪声
    uint   AO_NoiseTextureSize;       // 噪声贴图边长 (正方形)
    uint   AO_EnableBentNormal;       // 1=输出 bent normal
    float3 AO_BentNormalFallback;     // Fallback 值 (默认 0,0,1) 可由 CPU 设定
}

// 资源约定 (根据项目绑定调整 register)：
Texture2D<float>              GDepthTexture;             // 深度 (ReversedZ)
Texture2D<float4>             GNormalTexture;            // xyz:(-1,1) encoded normal
Texture2D<float2>             NoiseRotationTexture;      // RG = 旋转向量 (均匀单位圆)

RWTexture2D<float>            RWAORawTexture;            // 初始 AO (0~1, 1=无遮挡)
RWTexture2D<float>            RWAOIntermediateTexture;   // 横向模糊结果
RWTexture2D<float>            RWAOFinalTexture;          // 纵向模糊输出
// Bent Normal 输出：xyz = world-space 方向 (未编码)，w=1
// Raw / 中间 / 最终（可视需要绑定）
RWTexture2D<float4>           RWBentNormalRawTexture;          // Bent Normal Raw
RWTexture2D<float4>           RWBentNormalIntermediateTexture; // Bent Normal Blur X
RWTexture2D<float4>           RWBentNormalFinalTexture;        // Bent Normal Blur Y

// 采样器 (与项目现有保持一致命名) 
SamplerState PointClampSampler;
SamplerState LinearClampSampler;

// 工具函数 ---------------------------------------------------
float2 SpiralNoise(uint2 PixelCoords, uint FrameIndex, uint NoiseSize)
{
    if(NoiseSize == 0) return float2(1,0);
    uint2 n = PixelCoords % NoiseSize;
    float2 r = NoiseRotationTexture.Load(int3(n, 0));
    // 逐帧轻微旋转
    float angle = (FrameIndex * 0.6180339887f); // 黄金增量
    float s, c; sincos(angle, s, c);
    float2 rot = float2(c * r.x - s * r.y, s * r.x + c * r.y);
    return normalize(rot);
}

float3 DecodeNormal(float3 EncN)
{
    return normalize(EncN * 2.0f - 1.0f);
}

struct HBAOResult
{
    float Occlusion;     // [0,1] 1=无遮挡
    float3 BentNormal;   // world-space bent normal (unit)
};

HBAOResult ComputeHorizonAO(
    CameraParameters C,
    uint2 PixelCoords,
    float3 WorldPosition,
    float3 WorldNormal,
    float  LinearDepth,
    float2 ScreenDirection,
    float  Radius,
    float  StepDistribution,
    uint   StepCount,
    float  Bias,
    float  FalloffDistance)
{
    float2 FilmDimensions   = C.FilmDimensions;
    float2 UV               = (PixelCoords + 0.5f) * C.InvFilmDimensions;
    float2 PixelStepUV      = ScreenDirection / FilmDimensions;

    float  OcclusionAccum   = 0.0f;
    float  ValidSampleCount = 0.0f;
    float3 BentAccum        = 0.0f;
    float  BentWeightAccum  = 0.0f;
    [loop]
    for(uint StepIndex = 1; StepIndex <= StepCount; ++StepIndex)
    {
        float  T                  = (float)StepIndex / StepCount;
        float  DistributedT       = lerp(T, T * T, StepDistribution);
        float  SampleScreenRadius = DistributedT * min(Radius, AO_MaxScreenRadius);
        float2 SampleUV           = UV + PixelStepUV * SampleScreenRadius;
        if(any(SampleUV <= 0) || any(SampleUV >= 1)) continue;
        uint2  SamplePixelCoords  = uint2(SampleUV * FilmDimensions);
        float  SampleReversedZ    = GDepthTexture.SampleLevel(PointClampSampler, SampleUV, 0);
        if(SampleReversedZ <= 0) continue;
        float  SampleLinearDepth  = ReversedZDepthToLinearDepth(C, SampleReversedZ);

        float  DepthDelta         = SampleLinearDepth - LinearDepth;
        if(abs(DepthDelta) > FalloffDistance) continue;

        float3 SampleWorldPosition = RecoverWorldPositionPixelCoords(C, SamplePixelCoords, SampleLinearDepth);
        float3 ToSample            = SampleWorldPosition - WorldPosition;
        float  DistanceToSample    = length(ToSample);
        if(DistanceToSample < 1e-4f) continue;
        float3 DirectionWS         = ToSample / DistanceToSample;
        float  NDotDir             = dot(WorldNormal, DirectionWS);
        if(NDotDir <= Bias) continue;

        float  Falloff             = saturate(1.0f - (DistanceToSample / Radius));
        float  Contribution        = Falloff * (1.0f - NDotDir);
        OcclusionAccum += Contribution;
        ValidSampleCount += 1.0f;

        float  OpenWeight          = Falloff * max(NDotDir - Bias, 0.0f);
        BentAccum      += DirectionWS * OpenWeight;
        BentWeightAccum += OpenWeight;
    }
    HBAOResult Result;
    if(ValidSampleCount <= 0)
    {
        Result.Occlusion = 1.0f;
        Result.BentNormal = WorldNormal;
        return Result;
    }
    float RawAO = OcclusionAccum / ValidSampleCount;
    Result.Occlusion = saturate(pow(1.0f - RawAO * AO_Strength, AO_Power));
    Result.BentNormal = (BentWeightAccum <= 1e-6f) ? WorldNormal : normalize(BentAccum);
    return Result;
}

[numthreads(HBAO_THREAD_GROUP_SIZE_X, HBAO_THREAD_GROUP_SIZE_Y, 1)]
void GenerateHBAO (uint3 Gid : SV_DispatchThreadID)
{
    CameraParameters C = GetActiveCamera();
    uint2 PixelCoords = Gid.xy;
    if(PixelCoords.x >= (uint)C.FilmDimensions.x || PixelCoords.y >= (uint)C.FilmDimensions.y) return;

    float ReversedZDepth = GDepthTexture.Load(int3(PixelCoords,0));
    if(ReversedZDepth <= 0) { RWAORawTexture[PixelCoords] = 1.0f; return; }
    float  LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldNormal = DecodeNormal(GNormalTexture.Load(int3(PixelCoords,0)).xyz);
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelCoords, LinearDepth);

    float2 BaseRotation = SpiralNoise(PixelCoords, AO_FrameIndex, AO_NoiseTextureSize);
    float2 PerpRotation = float2(-BaseRotation.y, BaseRotation.x);

    uint   DirectionCount = max(1u, AO_NumDirections);
    float  AOAccum        = 0.0f;
    float3 BentAccum      = 0.0f;
    float  BentAccumW     = 0.0f;
    [loop]
    for(uint DirectionIndex = 0; DirectionIndex < DirectionCount; ++DirectionIndex)
    {
        float Angle = ((float)DirectionIndex + 0.5f) / DirectionCount * 3.14159265f;
        float2 ScreenDir = cos(Angle) * BaseRotation + sin(Angle) * PerpRotation;
        HBAOResult SampleResult = ComputeHorizonAO(C, PixelCoords, WorldPosition, WorldNormal, LinearDepth, ScreenDir, AO_Radius, AO_StepDistribution, AO_StepsPerDirection, AO_Bias, AO_FalloffDistance);
        AOAccum += SampleResult.Occlusion;
        if(AO_EnableBentNormal != 0) {
            BentAccum  += SampleResult.BentNormal;
            BentAccumW += 1.0f;
        }
    }
    float AmbientOcclusion = AOAccum / DirectionCount;
    RWAORawTexture[PixelCoords] = AmbientOcclusion;
    if(AO_EnableBentNormal != 0)
    {
        float3 BentNormal = (BentAccumW > 0 ? normalize(BentAccum / BentAccumW) : (AO_BentNormalFallback));
        RWBentNormalRawTexture[PixelCoords] = float4(BentNormal, 1.0f);
    }
}

float BilateralWeight(float centerDepth, float neighborDepth, float3 centerN, float3 neighborN, float depthSigma, float normalSigma)
{
    float dz = (neighborDepth - centerDepth);
    float wDepth = exp(- (dz*dz) / max(depthSigma*depthSigma, 1e-6f));
    float nd = saturate(dot(centerN, neighborN));
    float wNormal = pow(nd, normalSigma);
    return wDepth * wNormal;
}

[numthreads(HBAO_THREAD_GROUP_SIZE_X, HBAO_THREAD_GROUP_SIZE_Y, 1)]
void BilateralBlurHorizontal (uint3 Gid : SV_DispatchThreadID)
{
    CameraParameters C = GetActiveCamera();
    uint2 PixelCoords = Gid.xy;
    if(PixelCoords.x >= (uint)C.FilmDimensions.x || PixelCoords.y >= (uint)C.FilmDimensions.y) return;

    float ReversedZDepth = GDepthTexture.Load(int3(PixelCoords,0));
    if(ReversedZDepth <= 0) { RWAOIntermediateTexture[PixelCoords] = 1.0f; return; }
    float  LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldNormal = DecodeNormal(GNormalTexture.Load(int3(PixelCoords,0)).xyz);

    float  CenterAO    = RWAORawTexture[PixelCoords];
    float3 CenterBent  = (AO_EnableBentNormal != 0) ? RWBentNormalRawTexture[PixelCoords].xyz : 0.xxx;
    float  WeightSum   = 1.0f;
    float  AOWeighted  = CenterAO;
    float3 BentWeighted= CenterBent;

    float  BlurPixelRadius = AO_MaxScreenRadius * 0.5f;
    int    RadiusPixels    = (int)ceil(BlurPixelRadius);

    [loop]
    for(int dx = -RadiusPixels; dx <= RadiusPixels; ++dx)
    {
        if(dx == 0) continue;
        int2 nc = int2(PixelCoords) + int2(dx,0);
        if(nc.x < 0 || nc.y < 0 || nc.x >= (int)C.FilmDimensions.x || nc.y >= (int)C.FilmDimensions.y) continue;
        float ndReversed = GDepthTexture.Load(int3((uint2)nc,0));
        if(ndReversed <= 0) continue;
        float ndLinear      = ReversedZDepthToLinearDepth(C, ndReversed);
        float3 NeighborNormal = DecodeNormal(GNormalTexture.Load(int3((uint2)nc,0)).xyz);
        float NeighborAO    = RWAORawTexture[(uint2)nc];
        float3 NeighborBent = (AO_EnableBentNormal != 0) ? RWBentNormalRawTexture[(uint2)nc].xyz : 0.xxx;

        float W = BilateralWeight(LinearDepth, ndLinear, WorldNormal, NeighborNormal, AO_DepthSigma, AO_NormalSigma);
        W *= lerp(1.0f, saturate(1.0f - abs(ndLinear - LinearDepth) / (AO_DepthSigma*4)), AO_BlurEdgeSharpness);
        WeightSum  += W;
        AOWeighted += NeighborAO * W;
        if(AO_EnableBentNormal != 0) BentWeighted += NeighborBent * W;
    }
    float BlurredAO = AOWeighted / max(WeightSum, 1e-6f);
    RWAOIntermediateTexture[PixelCoords] = BlurredAO;
    if(AO_EnableBentNormal != 0)
    {
        float3 BentNormal = normalize(BentWeighted / max(WeightSum, 1e-6f));
        RWBentNormalIntermediateTexture[PixelCoords] = float4(BentNormal, 1.0f);
    }
}

[numthreads(HBAO_THREAD_GROUP_SIZE_X, HBAO_THREAD_GROUP_SIZE_Y, 1)]
void BilateralBlurVertical (uint3 Gid : SV_DispatchThreadID)
{
    CameraParameters C = GetActiveCamera();
    uint2 PixelCoords = Gid.xy;
    if(PixelCoords.x >= (uint)C.FilmDimensions.x || PixelCoords.y >= (uint)C.FilmDimensions.y) return;

    float ReversedZDepth = GDepthTexture.Load(int3(PixelCoords,0));
    if(ReversedZDepth <= 0) { RWAOFinalTexture[PixelCoords] = 1.0f; return; }
    float  LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldNormal = DecodeNormal(GNormalTexture.Load(int3(PixelCoords,0)).xyz);

    float  CenterAO    = RWAOIntermediateTexture[PixelCoords];
    float3 CenterBent  = (AO_EnableBentNormal != 0) ? RWBentNormalIntermediateTexture[PixelCoords].xyz : 0.xxx;
    float  WeightSum   = 1.0f;
    float  AOWeighted  = CenterAO;
    float3 BentWeighted= CenterBent;
    float  BlurPixelRadius = AO_MaxScreenRadius * 0.5f;
    int    RadiusPixels    = (int)ceil(BlurPixelRadius);

    [loop]
    for(int dy = -RadiusPixels; dy <= RadiusPixels; ++dy)
    {
        if(dy == 0) continue;
        int2 nc = int2(PixelCoords) + int2(0,dy);
        if(nc.x < 0 || nc.y < 0 || nc.x >= (int)C.FilmDimensions.x || nc.y >= (int)C.FilmDimensions.y) continue;
        float ndReversed = GDepthTexture.Load(int3((uint2)nc,0));
        if(ndReversed <= 0) continue;
        float ndLinear        = ReversedZDepthToLinearDepth(C, ndReversed);
        float3 NeighborNormal = DecodeNormal(GNormalTexture.Load(int3((uint2)nc,0)).xyz);
        float  NeighborAO     = RWAOIntermediateTexture[(uint2)nc];
        float3 NeighborBent   = (AO_EnableBentNormal != 0) ? RWBentNormalIntermediateTexture[(uint2)nc].xyz : 0.xxx;
        float  W              = BilateralWeight(LinearDepth, ndLinear, WorldNormal, NeighborNormal, AO_DepthSigma, AO_NormalSigma);
        W *= lerp(1.0f, saturate(1.0f - abs(ndLinear - LinearDepth) / (AO_DepthSigma*4)), AO_BlurEdgeSharpness);
        WeightSum  += W;
        AOWeighted += NeighborAO * W;
        if(AO_EnableBentNormal != 0) BentWeighted += NeighborBent * W;
    }
    float BlurredAO = AOWeighted / max(WeightSum, 1e-6f);
    RWAOFinalTexture[PixelCoords] = BlurredAO;
    if(AO_EnableBentNormal != 0)
    {
        float3 BentNormal = normalize(BentWeighted / max(WeightSum, 1e-6f));
        RWBentNormalFinalTexture[PixelCoords] = float4(BentNormal, 1.0f);
    }
}

// 调度顺序参考:
//   1) GenerateHBAO (FullRes 或 QuarterRes)
//   2) BilateralBlurHorizontal
//   3) BilateralBlurVertical
//   4) 合成阶段采样 RWAOFinalTexture 乘到漫反射
//   5) 若 AO_EnableBentNormal!=0，使用 RWBentNormalFinalTexture.xyz 作为 Bent Normal (world-space)

// 后续可扩展:
//   - 深度分层自适应方向/步数
//   - 多分辨率 (lower mip + upsample)
//   - Bent Normal 输出
//   - Hi-Z 早期终止
