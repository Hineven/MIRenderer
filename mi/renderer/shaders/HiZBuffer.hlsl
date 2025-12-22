/*
 * Created: 2025/7/24
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "headers/Camera.hlsl"
 #include "resources/CommonSamplerResources.hlsl"

#ifndef TILE_SIZE
// This can be overridden by compiler options.
#define TILE_SIZE 16
#endif

Texture2D<float> InDepthBuffer;
RWTexture2D<float> RWInHiZBuffer;
RWTexture2D<float> RWOutHiZBuffer;

Texture2D<uint> InFlagsBuffer;
[[vk::image_format("r8ui")]]
RWTexture2D<uint> RWInOrFlagsBuffer;
[[vk::image_format("r8ui")]]
RWTexture2D<uint> RWOutOrFlagsBuffer;

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void ComputeHiZBuffer(uint2 DispatchID : SV_DispatchThreadID)
{
    uint2 TexCoords = DispatchID;

    // Each thread computes one pixel in the output mip.
    // This corresponds to a 2x2 region in the input mip.
    uint2 InTexCoords = TexCoords * 2;
#ifndef DEPTH_AS_INPUT
    float D0 = RWInHiZBuffer[InTexCoords + uint2(0, 0)];
    float D1 = RWInHiZBuffer[InTexCoords + uint2(1, 0)];
    float D2 = RWInHiZBuffer[InTexCoords + uint2(0, 1)];
    float D3 = RWInHiZBuffer[InTexCoords + uint2(1, 1)];
    uint F0 = RWInOrFlagsBuffer[InTexCoords + uint2(0, 0)];
    uint F1 = RWInOrFlagsBuffer[InTexCoords + uint2(1, 0)];
    uint F2 = RWInOrFlagsBuffer[InTexCoords + uint2(0, 1)];
    uint F3 = RWInOrFlagsBuffer[InTexCoords + uint2(1, 1)];
#else
    uint2 HZBDimensions, DepthDimensions;
    HZBDimensions = GetActiveCamera().HZBDimensions;
    DepthDimensions = GetActiveCamera().FilmDimensions;
    // These functions seems to be broken
    // RWInHiZBuffer.GetDimensions(Dimensions.x, Dimensions.y);
    // InDepthBuffer.GetDimensions(DepthDimensions.x, DepthDimensions.y);
    float2 HZB_UV = (float2(TexCoords) + 0.25f) / float2(HZBDimensions);
    CameraParameters C = GetActiveCamera();
    float2 Depth_UV = HZB_UV * C.HZBToUVScale;
    float2 DeltaDepth_UV = C.HZBBaseTexelSize * C.HZBToUVScale * 0.5f;
    float2 P0 = Depth_UV;
    float2 P1 = Depth_UV + float2(DeltaDepth_UV.x, 0);
    float2 P2 = Depth_UV + float2(0, DeltaDepth_UV.y);
    float2 P3 = Depth_UV + DeltaDepth_UV;

    float D0 = InDepthBuffer.SampleLevel(PointEdgeSampler, P0, 0);
    float D1 = InDepthBuffer.SampleLevel(PointEdgeSampler, P1, 0);
    float D2 = InDepthBuffer.SampleLevel(PointEdgeSampler, P2, 0);
    float D3 = InDepthBuffer.SampleLevel(PointEdgeSampler, P3, 0);
    if(any(P0 >= 1.f)) D0 = 1.f;
    if(any(P1 >= 1.f)) D1 = 1.f;
    if(any(P2 >= 1.f)) D2 = 1.f;
    if(any(P3 >= 1.f)) D3 = 1.f;
    uint2 P0u = uint2(P0 * DepthDimensions);
    uint2 P1u = uint2(P1 * DepthDimensions);
    uint2 P2u = uint2(P2 * DepthDimensions);
    uint2 P3u = uint2(P3 * DepthDimensions);
    uint F0 = InFlagsBuffer.Load(uint3(P0u, 0)).r;
    uint F1 = InFlagsBuffer.Load(uint3(P1u, 0)).r;
    uint F2 = InFlagsBuffer.Load(uint3(P2u, 0)).r;
    uint F3 = InFlagsBuffer.Load(uint3(P3u, 0)).r;
#endif
    // We want the max depth (furthest away).
    // In many depth buffer setups (like reversed-Z), this means the maximum float value.
    float MaxDepth = max(max(D0, D1), max(D2, D3));

    RWOutHiZBuffer[TexCoords] = MaxDepth;

    // We want the bitwise OR of the flags.
    uint OrFlags = F0 | F1 | F2 | F3;
    RWOutOrFlagsBuffer[TexCoords] = OrFlags;
}
