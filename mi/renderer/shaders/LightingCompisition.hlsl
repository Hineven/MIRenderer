#include "headers/CommonSamplers.hlsl"
#include "headers/Camera.hlsl"

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

Texture2D<float4> DiffuseDirectLightingTexture;
Texture2D<float4> HistoryDiffuseDirectLightingTexture;

Texture2D<float4> G_Albedo;

RWTexture2D<float4> RWRadiance;

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void LightingComposition(uint2 DispatchID : SV_DispatchThreadID)
{
    uint2 PixelIndex = DispatchID;
    CameraParameters C = GetActiveCamera();
    if (any(PixelIndex >= C.FilmDimensions)) return;

    float2 UV = ScreenCoordsToUV(C, PixelIndex);
    float4 Albedo = G_Albedo.SampleLevel(PointClampSampler, UV, 0);

    float3 DiffuseDirectLighting = DiffuseDirectLightingTexture.SampleLevel(PointClampSampler, UV, 0).rgb;
    
    RWRadiance[PixelIndex] = float4(DiffuseDirectLighting * Albedo.rgb, 1.0f);
}