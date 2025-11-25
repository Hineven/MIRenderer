// Simple placeholder shader for Gaussian Radiance Field splatting
#include "shared/SharedCommon.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedGaussianRadianceField.hlsl"
#include "shared/SharedView.hlsl"
#include "headers/Camera.hlsl"
#include "resources/CommonSamplerResources.hlsl"

#ifndef THREAD_GROUP_SIZE
#define THREAD_GROUP_SIZE 128
#endif

struct DrawGaussianRadianceFieldUB {
    uint NumPoints;
    uint3 Padding;
};
ConstantBuffer<DrawGaussianRadianceFieldUB> UB;
StructuredBuffer<GaussianRadianceFieldHeader> FieldHeaderBuffer; // currently unused
StructuredBuffer<PackedGaussianRadiance> PointDataBuffer;
[[vk::image_format("rgba16f")]] RWTexture2D<float4> RWRadiance;
Texture2D G_Depth; // unused placeholder
SamplerState PointEdgeSampler;

float3 UnpackColor(uint packedColorOpacityLo) {
    uint rgb = (packedColorOpacityLo & 0x00FFFFFFu);
    uint r8 = (rgb & 0x000000FFu);
    uint g8 = (rgb & 0x0000FF00u) >> 8;
    uint b8 = (rgb & 0x00FF0000u) >> 16;
    return float3(r8, g8, b8) / 255.0f;
}

[numthreads(THREAD_GROUP_SIZE,1,1)]
void DrawGaussianRadianceField(uint3 DTid : SV_DispatchThreadID) {
    uint id = DTid.x;
    if (id >= UB.NumPoints) return;
    PackedGaussianRadiance p = PointDataBuffer[id];
    float3 color = UnpackColor(p.PackedColor_OpacityLo);
    // Naive projection: write to center pixel (no proper splat yet)
    CameraParameters C = GetActiveCamera();
    float4 world = float4(p.Position,1);
    float4 clip = mul(C.WorldToNDC, world);
    float2 ndc = clip.xy / clip.w;
    float2 screen = 0.5 * (ndc + 1.0) * C.FilmDimensions;
    uint2 pix = (uint2)screen;
    if (pix.x >= C.FilmDimensions.x || pix.y >= C.FilmDimensions.y) return;
    float4 prev = RWRadiance[pix];
    RWRadiance[pix] = prev + float4(color,1);
}

