#include "headers/Camera.hlsl"
#include "headers/Conversions.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "resources/CommonSamplerResources.hlsl"

struct TAAUB {
    float2 FilmDimensions;
    float  Padding0;
    float  Padding1;
};
ConstantBuffer<TAAUB> UB;

Texture2D<float4> CurrentRadianceTexture; // linear radiance
Texture2D<float4> PreviousRadianceTexture; // linear radiance (history)
Texture2D<float2> G_MotionVector; // NDC delta (current - prev)
Texture2D<float>  G_Depth;
Texture2D<float4> G_Normal; // packed in 0..1

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWRadianceTexture;

float3 Map(float3 color) {
    // Reinhard tone mapping
    return color / (dot(color, float3(0.2126f, 0.7152f, 0.0722f)) + 1.0f);
}

float3 ReverseMap(float3 color) {
    // Reverse Reinhard tone mapping
    return color / (1.0f - dot(color, float3(0.2126f, 0.7152f, 0.0722f)));
}

float4 ClampHistory(float4 hist, float2 uv) {
    // Gather 3x3 neighborhoo statistics
    float3 m1 = 0, m2 = 0;
    float min_a = 1.0f, max_a = 0.0f; // Use Min/Max for Alpha (AABB)

    [unroll]
    for(int j=-1;j<=1;j++) {
        [unroll]
        for(int i=-1;i<=1;i++) {
            float2 uvn = uv + float2(i,j) / UB.FilmDimensions;
            float4 Sample = CurrentRadianceTexture.SampleLevel(PointEdgeSampler, uvn, 0);
            float3 c = Map(Sample.rgb);
            float3 yc = RGBToYCoCg(c.rgb);
            m1 += yc;
            m2 += yc * yc;
            
            // For Alpha, strict Min/Max is often more stable than Variance for binary masks
            min_a = min(min_a, Sample.w);
            max_a = max(max_a, Sample.w);
        }
    }
    float3 mu = m1 / 9.0f;
    float3 sigma = sqrt(abs(m2 / 9.0f - mu * mu));
    
    float gamma = 2.5f;
    float3 minN = mu - gamma * sigma;
    float3 maxN = mu + gamma * sigma;

    float4 clamped;
    clamped.rgb = clamp(hist.rgb, minN, maxN);
    clamped.w = clamp(hist.w, min_a, max_a);
    return clamped;
}

float2 GetClosestDepthMotionVector(int2 tid) {
    float MaxDepth = 0.f;
    float2 bestMV = float2(0.0f, 0.0f);
    int2 dims = int2(UB.FilmDimensions);

    [unroll]
    for(int j=-1; j<=1; ++j) {
        [unroll]
        for(int i=-1; i<=1; ++i) {
            int2 p = tid + int2(i,j);
            if(any(p < 0) || any(p >= dims)) continue;

            float d = G_Depth.Load(uint3(p, 0));
            // Pick closest (largest) depth motion vector (using reversed-Z)
            if(d > MaxDepth) {
                MaxDepth = d;
                bestMV = G_MotionVector.Load(uint3(p, 0));
            }
        }
    }
    return bestMV;
}


// Source: https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
// License: https://gist.github.com/TheRealMJP/bc503b0b87b643d3505d41eab8b332ae
float4 SampleHistoryCatmullRom(in float2 uv, in float2 texelSizeUV)
{
    // We're going to sample a a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
    // down the sample location to get the exact center of our "starting" texel. The starting texel will be at
    // location [1, 1] in the grid, where [0, 0] is the top left corner.
    float2 samplePos = uv / texelSizeUV;
    float2 texPos1   = floor(samplePos - 0.5f) + 0.5f;

    // Compute the fractional offset from our starting texel to our original sample location, which we'll
    // feed into the Catmull-Rom spline function to get our filter weights.
    float2 f = samplePos - texPos1;

    // Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
    // These equations are pre-expanded based on our knowledge of where the texels will be located,
    // which lets us avoid having to evaluate a piece-wise function.
    float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    float2 w3 = f * f * (-0.5f + 0.5f * f);

    // Work out weighting factors and sampling offsets that will let us use bilinear filtering to
    // simultaneously evaluate the middle 2 samples from the 4x4 grid.
    float2 w12      = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);

    // Compute the final UV coordinates we'll use for sampling the texture
    float2 texPos0  = texPos1 - 1.0f;
    float2 texPos3  = texPos1 + 2.0f;
    float2 texPos12 = texPos1 + offset12;

    texPos0  *= texelSizeUV;
    texPos3  *= texelSizeUV;
    texPos12 *= texelSizeUV;

    float4 result = 0;

    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos0.x,  texPos0.y),  0.0f) * w0.x  * w0.y;
    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos12.x, texPos0.y),  0.0f) * w12.x * w0.y;
    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos3.x,  texPos0.y),  0.0f) * w3.x  * w0.y;

    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos0.x,  texPos12.y), 0.0f) * w0.x  * w12.y;
    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos12.x, texPos12.y), 0.0f) * w12.x * w12.y;
    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos3.x,  texPos12.y), 0.0f) * w3.x  * w12.y;

    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos0.x,  texPos3.y),  0.0f) * w0.x  * w3.y;
    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos12.x, texPos3.y),  0.0f) * w12.x * w3.y;
    result += PreviousRadianceTexture.SampleLevel(LinearEdgeSampler, float2(texPos3.x,  texPos3.y),  0.0f) * w3.x  * w3.y;

    return max(result, 0.0f);
}

[numthreads(8,8,1)]
void TAA_Main(uint2 tid: SV_DispatchThreadID)
{
    CameraParameters C = GetActiveCamera();
    if(any(tid >= C.FilmDimensions)) return;
    float2 UV = (tid + 0.5f) * C.InvFilmDimensions;
    float4 CurrentSample = CurrentRadianceTexture.Load(uint3(tid,0));
    float3 CurrRadiance  = CurrentSample.rgb;
    
    // Use velocity dilation to find the most prominent motion in the neighborhood
    float2 mv = GetClosestDepthMotionVector(int2(tid));
    
    // Standard TAA reprojection
    float2 DeltaJitter = 0; // Reproject in non-jittered space
    float2 PrevUV = UV - 0.5f * float2(mv.x, -mv.y) - 0.5f * float2(DeltaJitter.x, -DeltaJitter.y);

    // Reject if history is off-screen
    if(any(PrevUV < 0.0f) || any(PrevUV > 1.0f)) {
        RWRadianceTexture[tid] = CurrentSample;
        return;
    }


    float4 PrevSample = SampleHistoryCatmullRom(PrevUV, C.InvFilmDimensions);
    float3 PrevRadiance = PrevSample.rgb;
    // Compress to tone-mapped space for better clamping and ghosting removal for bright areas
    float3 PrevRGB = Map(PrevRadiance);
    float3 CurrRGB = Map(CurrRadiance);

    // Simple neighborhood clamp in YCoCg
    float3 CurrYCoCg = RGBToYCoCg(CurrRGB);
    float3 PrevYCoCg = RGBToYCoCg(PrevRGB);
    
    // Combine YCoCg and Alpha into one vector for clamping
    float4 HistVec = float4(PrevYCoCg, PrevSample.w);
    float4 ClampedHist = ClampHistory(HistVec, UV);

    float4 TargetVec = float4(CurrYCoCg, CurrentSample.w);
    float4 OutVec = lerp(TargetVec, ClampedHist, 1.f - 1.0f / 8.0f);

    float3 OutRGB = YCoCgToRGB(OutVec.rgb);
    float3 OutRadiance = ReverseMap(OutRGB);
    RWRadianceTexture[tid] = float4(OutRadiance, OutVec.w);
}

