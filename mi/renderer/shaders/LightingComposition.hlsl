#include "resources/CommonSamplerResources.hlsl"
#include "headers/Camera.hlsl"
#include "headers/Scattering.hlsl"

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

struct LightingCompositionUB {
    uint EnableAccumulation;
    uint Padding0;
    uint Padding1;
    uint Padding2;
};

ConstantBuffer<LightingCompositionUB> UB;

Texture2D<float4> DiffuseDirectLightingTexture;
Texture2D<float4> DiffuseIndirectLightingTexture;
Texture2D<float4> VolumeDirectLightingTexture;
//Texture2D<float4> HistoryDiffuseDirectLightingTexture;

TextureCube<float4> EnvironmentMap;

Texture2D<float4> G_Albedo;
Texture2D<float4> G_Emission;
Texture2D<float>  G_Transmittance;

Texture2D<float4> HistoryRadiance;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWRadiance;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWShadedRadianceWithoutEmission;

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void LightingComposition(uint2 DispatchID : SV_DispatchThreadID)
{
    uint2 PixelIndex = DispatchID;
    CameraParameters C = GetActiveCamera();
    if (any(PixelIndex >= C.FilmDimensions)) return;

    float2 UV = ScreenCoordsToUV(C, PixelIndex);
    float4 AlbedoAlpha = G_Albedo.SampleLevel(PointEdgeSampler, UV, 0);

    float3 SurfaceRadiance = 0;

    // Emission
    float3 Emission = G_Emission.SampleLevel(PointEdgeSampler, UV, 0).rgb;
    if(AlbedoAlpha.w == 0.f) {
        float3 RayDirection = NDC2ToCameraDirection(C, UVToNDC2(UV));
        float3 EnvironmentColor = EnvironmentMap.SampleLevel(LinearWrapSampler, -RayDirection, 0).xyz;
        Emission = EnvironmentColor;
    }

    // Diffuse direct
    float3 DiffuseDirectLighting = DiffuseDirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;
    SurfaceRadiance += DiffuseDirectLighting * EvaluateLambert(AlbedoAlpha.rgb);

    // Diffuse indirect
    float3 DiffuseIndirectLighting = DiffuseIndirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;
    SurfaceRadiance += DiffuseIndirectLighting * EvaluateLambert(AlbedoAlpha.rgb);

    // Color is premultiplied.
    float3 VolumeDirectLighting = VolumeDirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;
    float3 VolumeRadiance = VolumeDirectLighting;

    float Transmittance = G_Transmittance.SampleLevel(PointEdgeSampler, UV, 0);

    float3 Radiance = (Emission + SurfaceRadiance) * Transmittance + VolumeRadiance;

    float3 OldRadiance = HistoryRadiance.SampleLevel(PointEdgeSampler, UV, 0).rgb;
    float LerpFactor = 0.01f;
    if(UB.EnableAccumulation == 0) {
        OldRadiance = 0;
        LerpFactor = 1;
    }

    RWRadiance[PixelIndex] = float4(lerp(OldRadiance, Radiance, LerpFactor), 1.0f);

    // Specially for screen space radiance reuse
    RWShadedRadianceWithoutEmission[PixelIndex] = float4(SurfaceRadiance, 1);
}