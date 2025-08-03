#include "resources/CommonSamplerResources.hlsl"
#include "headers/Camera.hlsl"

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

Texture2D<float4> DiffuseDirectLightingTexture;
//Texture2D<float4> HistoryDiffuseDirectLightingTexture;

Texture2D<float4> G_Albedo;
Texture2D<float4> G_Emission;

RWTexture2D<float4> RWRadiance;

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void LightingComposition(uint2 DispatchID : SV_DispatchThreadID)
{
    uint2 PixelIndex = DispatchID;
    CameraParameters C = GetActiveCamera();
    if (any(PixelIndex >= C.FilmDimensions)) return;

    float2 UV = ScreenCoordsToUV(C, PixelIndex);
    float4 AlbedoAlpha = G_Albedo.SampleLevel(PointClampSampler, UV, 0);

    float3 DiffuseDirectLighting = DiffuseDirectLightingTexture.SampleLevel(PointClampSampler, UV, 0).rgb;

    float3 Emission = G_Emission.SampleLevel(PointClampSampler, UV, 0).rgb;
    if(AlbedoAlpha.w == 0.f) Emission = 0;

    float3 Radiance = Emission;

    Radiance += DiffuseDirectLighting * AlbedoAlpha.rgb;

    RWRadiance[PixelIndex] = float4(Radiance, 1.0f);
}