/*
 * Created: 2025/7/24
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef TILE_SIZE
// This can be overridden by compiler options.
#define TILE_SIZE 16
#endif

RWTexture2D<float> RWInHiZBuffer;
RWTexture2D<float> RWOutHiZBuffer;

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void ComputeHiZBuffer(uint3 DispatchID : SV_DispatchThreadID)
{
    uint2 TexCoords = DispatchID.xy;

    // Each thread computes one pixel in the output mip.
    // This corresponds to a 2x2 region in the input mip.
    uint2 InTexCoords = TexCoords * 2;

    float D0 = RWInHiZBuffer[InTexCoords + uint2(0, 0)];
    float D1 = RWInHiZBuffer[InTexCoords + uint2(1, 0)];
    float D2 = RWInHiZBuffer[InTexCoords + uint2(0, 1)];
    float D3 = RWInHiZBuffer[InTexCoords + uint2(1, 1)];

    // We want the max depth (furthest away).
    // In many depth buffer setups (like reversed-Z), this means the maximum float value.
    float MaxDepth = max(max(D0, D1), max(D2, D3));

    RWOutHiZBuffer[TexCoords] = MaxDepth;
}
