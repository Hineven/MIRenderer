#include "shared/SharedView.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Math.hlsl"
#include "headers/Radiometry.hlsl"
#include "headers/GeometryBuffers.hlsl"

#include "resources/HashGridCacheResources.hlsl"

Texture2D<float> G_Depth;
Texture2D<float4> PreviousShadedDiffuseRadianceWithoutEmission;
RWTexture2D<float4> RWDebugOutputTexture;

[numthreads(8, 8, 1)]
void VisualizeWorldCache (uint2 DispatchID : SV_DispatchThreadID) {
    uint2 ScreenCoords = DispatchID;
    CameraParameters C = GetActiveCamera();
    if(any(ScreenCoords >= C.FilmDimensions)) return;
    float ReversedZDepth = G_Depth.Load(int3(ScreenCoords, 0));
    if(ReversedZDepth == 0) {
        RWDebugOutputTexture[ScreenCoords] = float4(0, 0, 0, 1);
        return;
    }
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, ScreenCoords, LinearDepth);

    float3 Radiance = 0;

    float3 ViewDirection = normalize(C.Position - WorldPosition);
    HashGridsKey Key = HashGrids_GetEntryKey(WorldPosition, ViewDirection);
    uint HashGridSlot = HashGrids_Find(Key.BucketHash);
    bool bRadianceFound = false;
    if(IsValid(HashGridSlot)) {
        uint TileIndex = HashGrids_BucketTileIndexBuffer[HashGridSlot];
        if(IsValid(TileIndex)) {
            uint CellIndex  = HashGrids_GetCellIndex(TileIndex, Key.CellOffset);
            Radiance = HashGrids_GetFilteredRadiance(CellIndex).xyz;
            bRadianceFound = true;
        }
    }
    if(!bRadianceFound) {
        // Fallback to prev radiance
        Radiance = PreviousShadedDiffuseRadianceWithoutEmission.Load(int3(ScreenCoords, 0)).xyz;
    }
    RWDebugOutputTexture[ScreenCoords] = float4(Radiance, 1);
}