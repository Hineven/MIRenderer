#include "shared/SharedView.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Math.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "resources/CommonSamplerResources.hlsl"

Texture2D<float> G_Depth;

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDebugOutputTexture;

StructuredBuffer<float3> SpatialPositionsBuffer;
StructuredBuffer<uint> SpatialPositionsCount;

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void VisualizeSpatialPositions (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID >= SpatialPositionsCount[0]) return;
    float3 Position = SpatialPositionsBuffer[DispatchID];
    CameraParameters C = GetActiveCamera();
    float3 NDC = TransformPoint(C.WorldToNDC, Position);
    float2 UV = NDC2ToUV(NDC.xy);
    if(any(UV <= 0.f.xx) || any(UV >= 1.f.xx)) {
        // Outside of screen
        return ;
    }
    float ReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, UV, 0);
    float ZDepth = 1.f - ReversedZDepth;
    if(ZDepth < NDC.z) {
        // Occluded
        return ;
    }
    float3 CloseColor = float3(1.f, 0.f, 0.f);
    float3 FarColor = float3(0.f, 1.f, 0.f);
    float3 Color = lerp(CloseColor, FarColor, NDC.z);
    int2 PixelCoords = int2(UV * C.FilmDimensions);
    int MaxRadius = 1;
    for(int dX = -MaxRadius; dX <= MaxRadius; dX++) {
        for(int dY = -MaxRadius; dY <= MaxRadius; dY++) {
            int2 WritingCoords = PixelCoords + int2(dX, dY);
            if(all(WritingCoords >= 0) && all(WritingCoords < C.FilmDimensions)) {
                RWDebugOutputTexture[WritingCoords] = float4(Color, 1.f);
            }
        }
    }
}