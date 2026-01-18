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

float4 ClampHistory(float4 hist, float2 uv) {
    // Gather 3x3 neighborhoo statistics
    float3 m1 = 0, m2 = 0;
    float min_a = 1.0f, max_a = 0.0f; // Use Min/Max for Alpha (AABB)

    [unroll]
    for(int j=-1;j<=1;j++) {
        [unroll]
        for(int i=-1;i<=1;i++) {
            float2 uvn = uv + float2(i,j) / UB.FilmDimensions;
            float4 c = CurrentRadianceTexture.SampleLevel(PointEdgeSampler, uvn, 0);
            float3 yc = RGBToYCoCg(c.rgb);
            m1 += yc;
            m2 += yc * yc;
            
            // For Alpha, strict Min/Max is often more stable than Variance for binary masks
            min_a = min(min_a, c.w);
            max_a = max(max_a, c.w);
        }
    }
    float3 mu = m1 / 9.0f;
    float3 sigma = sqrt(abs(m2 / 9.0f - mu * mu));
    
    float gamma = 1.5f;
    float3 minN = mu - gamma * sigma;
    float3 maxN = mu + gamma * sigma;

    float4 clamped;
    clamped.rgb = clamp(hist.rgb, minN, maxN);
    clamped.w = clamp(hist.w, min_a, max_a);
    return clamped;
}

float2 GetClosestDepthMotionVector(int2 tid) {
    float MinDepth = 1.0f;
    float2 bestMV = float2(0.0f, 0.0f);
    int2 dims = int2(UB.FilmDimensions);

    [unroll]
    for(int j=-1; j<=1; ++j) {
        [unroll]
        for(int i=-1; i<=1; ++i) {
            int2 p = tid + int2(i,j);
            if(any(p < 0) || any(p >= dims)) continue;

            float d = G_Depth.Load(uint3(p, 0));
            if(d != 0 && d < MinDepth) {
                MinDepth = d;
                bestMV = G_MotionVector.Load(uint3(p, 0));
            }
        }
    }
    return bestMV;
}

[numthreads(8,8,1)]
void TAA_Main(uint2 tid: SV_DispatchThreadID)
{
    CameraParameters C = GetActiveCamera();
    if(any(tid >= C.FilmDimensions)) return;
    float2 uv = (tid + 0.5f) * C.InvFilmDimensions;
    float4 CurrentSample = CurrentRadianceTexture.Load(uint3(tid,0));
    float3 CurrRGB = CurrentSample.rgb;
    
    // Use velocity dilation to find the most prominent motion in the neighborhood
    float2 mv = GetClosestDepthMotionVector(int2(tid));
    
    // Standard TAA reprojection
    float2 PrevUV = uv - float2(mv.x * 0.5, -mv.y * 0.5);

    // Reject if history is off-screen
    if(any(PrevUV < 0.0f) || any(PrevUV > 1.0f)) {
        RWRadianceTexture[tid] = CurrentSample;
        return;
    }

    float4 PrevSample = PreviousRadianceTexture.SampleLevel(PointEdgeSampler, PrevUV, 0);
    float3 PrevRGB = PrevSample.rgb;

    // Simple neighborhood clamp in YCoCg
    float3 CurrYCoCg = RGBToYCoCg(CurrRGB);
    float3 PrevYCoCg = RGBToYCoCg(PrevRGB);
    
    // Combine YCoCg and Alpha into one vector for clamping
    float4 HistVec = float4(PrevYCoCg, PrevSample.w);
    float4 ClampedHist = ClampHistory(HistVec, uv);

    float4 TargetVec = float4(CurrYCoCg, CurrentSample.w);
    float4 OutVec = lerp(TargetVec, ClampedHist, UB.BlendFactor);

    float3 OutRGB = YCoCgToRGB(OutVec.rgb);
    RWRadianceTexture[tid] = float4(OutRGB, OutVec.w);
}

