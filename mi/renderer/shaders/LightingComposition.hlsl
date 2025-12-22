#include "headers/Camera.hlsl"
#include "headers/Scattering.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/EnvironmentLightResource.hlsl"

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

struct LightingCompositionUB {
    uint EnableAccumulation;
    uint EnableDiffuseDirect;
    uint EnableDiffuseIndirect;
    uint EnableVolumeDirect;
    uint EnableVolumeIndirect;
    uint EnableVolumeGridDirect;
    uint Padding0;
    uint Padding1;
};

ConstantBuffer<LightingCompositionUB> UB;

Texture2D<float4> DiffuseDirectLightingTexture;
Texture2D<float4> DiffuseIndirectLightingTexture;
Texture2D<float4> VolumeDirectLightingTexture;
Texture2D<float4> VolumeIndirectLightingTexture;
Texture2D<float4> VolumeGridDirectLightingTexture;

Texture2D<float4> G_Albedo;
Texture2D<float4> G_Emission;
Texture2D<float>  G_Transmittance;

Texture2D<float4> HistoryRadiance;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWRadiance;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWShadedRadianceWithoutEmission;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWShadedVolumeRadiance;

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
        float3 EnvironmentColor = EvaluateEnvironmentMap(-RayDirection);
        Emission = EnvironmentColor;
    }

    // Diffuse direct
    float3 VisualizeDiffuseDirectLighting = UB.EnableDiffuseDirect != 0 ? DiffuseDirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb : 0;
    float3 DiffuseDirectLighting = DiffuseDirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;
    // Diffuse indirect
    float3 VisualizeDiffuseIndirectLighting = UB.EnableDiffuseIndirect != 0 ? DiffuseIndirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb : 0;
    float3 DiffuseIndirectLighting = DiffuseIndirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;

    float3 VisualizeSurfaceRadiance = (VisualizeDiffuseDirectLighting + VisualizeDiffuseIndirectLighting) * EvaluateLambert(AlbedoAlpha.rgb);

    SurfaceRadiance += DiffuseDirectLighting * EvaluateLambert(AlbedoAlpha.rgb);
    SurfaceRadiance += DiffuseIndirectLighting * EvaluateLambert(AlbedoAlpha.rgb);

    // Volume direct
    float3 VisualizeVolumeDirectLighting = UB.EnableVolumeDirect != 0 ? VolumeDirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb : 0;
    float3 VolumeDirectLighting = VolumeDirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;
    // Volume indirect
    float3 VisualizeVolumeIndirectLighting = UB.EnableVolumeIndirect != 0 ? VolumeIndirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb : 0;
    float3 VolumeIndirectLighting = VolumeIndirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;

    float3 VisualizeVolumeLighting = VisualizeVolumeDirectLighting + VisualizeVolumeIndirectLighting;
    float3 VolumeRadiance = VolumeDirectLighting + VolumeIndirectLighting;

    // Volume grid direct
    float3 VisualizeVolumeGridDirectLighting = UB.EnableVolumeGridDirect != 0 ? VolumeGridDirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb : 0;
    float3 VolumeGridDirectLighting = VolumeDirectLightingTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;

    float3 VisualizeVolumeGridLighting = VisualizeVolumeGridDirectLighting;
    float3 VolumeGridRadiance = VolumeGridDirectLighting;

    float Transmittance = G_Transmittance.SampleLevel(PointEdgeSampler, UV, 0);

    float3 VisualizeRadiance = (Emission + VisualizeSurfaceRadiance) * Transmittance + VisualizeVolumeLighting + VisualizeVolumeGridLighting;
    float3 Radiance = (Emission + SurfaceRadiance) * Transmittance + VolumeRadiance + VolumeGridRadiance;

    float3 OldVisualizeRadiance = HistoryRadiance.SampleLevel(PointEdgeSampler, UV, 0).rgb;
    float LerpFactor = 0.01f;
    if(UB.EnableAccumulation == 0) {
        OldVisualizeRadiance = 0;
        LerpFactor = 1;
    }

    RWRadiance[PixelIndex] = float4(lerp(OldVisualizeRadiance, VisualizeRadiance, LerpFactor), 1.0f);

    // Specially for screen space radiance reuse
    RWShadedRadianceWithoutEmission[PixelIndex] = float4(SurfaceRadiance, 1);
    RWShadedVolumeRadiance[PixelIndex] = float4(VolumeRadiance, 1);
}