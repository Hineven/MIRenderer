#include "headers/Camera.hlsl"
#include "headers/Conversions.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "resources/CommonSamplerResources.hlsl"

struct TAAUB {
    float2 FilmDimensions;
    float  BlendFactor; // history weight
    float  Padding0;
};
ConstantBuffer<TAAUB> UB;

Texture2D<float4> CurrentRadianceTexture; // linear radiance
Texture2D<float4> PreviousRadianceTexture; // linear radiance (history)
Texture2D<float2> G_MotionVector; // NDC delta (current - prev)
Texture2D<float>  G_Depth;
Texture2D<float4> G_Normal; // packed in 0..1

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWRadianceTexture;

float3 ClampHistoryYCoCg(float3 curr, float3 hist, float2 uv) {
    // Gather 3x3 neighborhoo statistics
    float3 m1 = 0, m2 = 0;
    [unroll]
    for(int j=-1;j<=1;j++) {
        [unroll]
        for(int i=-1;i<=1;i++) {
            float2 uvn = uv + float2(i,j) / UB.FilmDimensions;
            float3 c = CurrentRadianceTexture.SampleLevel(PointEdgeSampler, uvn, 0).rgb;
            float3 yc = RGBToYCoCg(c);
            m1 += yc;
            m2 += yc * yc;
        }
    }
    float3 mu = m1 / 9.0f;
    float3 sigma = sqrt(abs(m2 / 9.0f - mu * mu));
    float gamma = 1.5f;
    float3 minN = mu - gamma * sigma;
    float3 maxN = mu + gamma * sigma;
    float3 clamped = clamp(hist, minN, maxN);
    return clamped;
}

[numthreads(8,8,1)]
void TAA_Main(uint2 tid: SV_DispatchThreadID)
{
    CameraParameters C = GetActiveCamera();
    if(any(tid >= C.FilmDimensions)) return;
    float2 uv = (tid + 0.5f) * C.InvFilmDimensions;
    float4 CurrentSample = CurrentRadianceTexture.Load(uint3(tid,0));
    float3 currRGB = CurrentSample.rgb;
    // Reject if no geometry
    float Depth = G_Depth.Load(uint3(tid,0));
    if(Depth == 0) {
        RWRadianceTexture[tid] = CurrentSample;
        return;
    }

    float2 mv = G_MotionVector.Load(uint3(tid,0));
    float2 prevUV = uv - float2(mv.x * 0.5, -mv.y * 0.5);

    float3 prevRGB = PreviousRadianceTexture.SampleLevel(PointEdgeSampler, prevUV, 0).rgb;

    // Simple neighborhood clamp in YCoCg
    float3 currYCg = RGBToYCoCg(currRGB);
    float3 prevYCg = RGBToYCoCg(prevRGB);
    float3 clampedHist = ClampHistoryYCoCg(currRGB, prevYCg, uv);

    float3 ycOut = lerp(currYCg, clampedHist, UB.BlendFactor);
    float3 outRGB = YCoCgToRGB(ycOut);
    RWRadianceTexture[tid] = float4(outRGB, CurrentSample.w);
}

