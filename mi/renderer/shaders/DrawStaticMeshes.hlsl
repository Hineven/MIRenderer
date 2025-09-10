#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Math.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"

StructuredBuffer<uint2>            RenderableIndexAndDescriptorIndexBuffer;

struct DrawDeferredStaticMeshesVSOut {
    float4 Position : SV_POSITION;
    uint   DescriptorRenderableIndex : TEXCOORD0; // Packed DescriptorIndex (8bits) RenderableIndex (24bits)
};

DrawDeferredStaticMeshesVSOut DrawDeferredStaticMeshesVS (DefaultStaticMeshVertex Vertex, uint InstanceIndex : SV_InstanceID) {
    uint2 RenderableIndex_DescriptorIndex = RenderableIndexAndDescriptorIndexBuffer[InstanceIndex];
    uint RenderableIndex = RenderableIndex_DescriptorIndex.x;
    uint DescriptorIndex = RenderableIndex_DescriptorIndex.y;
    float3x4 ToWorldTransform = RenderableTransformBuffer[RenderableIndex];
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    float4 PositionW = mul(View.Camera.WorldToNDC_ReversedZ, float4(WorldPosition, 1));

    DrawDeferredStaticMeshesVSOut Output = (DrawDeferredStaticMeshesVSOut)0;
    Output.Position = PositionW;
    Output.DescriptorRenderableIndex = DescriptorIndex << 24 | RenderableIndex;
    return Output;
}


struct DrawDeferredStaticMeshesPSOut {
    uint4 Visibility : SV_TARGET0;
};

DrawDeferredStaticMeshesPSOut DrawDeferredStaticMeshesPS (
    DrawDeferredStaticMeshesVSOut Input,
    uint PrimitiveIndex : SV_PrimitiveID,
    float2 Barycentrics : SV_BaryCentrics
) {
    DrawDeferredStaticMeshesPSOut Output = (DrawDeferredStaticMeshesPSOut)0;
    Output.Visibility = uint4(
        Input.DescriptorRenderableIndex, 
        PrimitiveIndex,
        asuint(Barycentrics.x),
        asuint(Barycentrics.y)
    );
    return Output;
}

RWTexture2D<float4> RWAlbedo;
RWTexture2D<float4> RWNormal;
RWTexture2D<float4> RWEmission;
RWTexture2D<float2> RWMetallicRoughness;
Texture2D<uint4> VisibilityTexture;
Texture2D<uint4> DepthTexture;

#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void DecodeVisibility (uint2 DispatchThreadID : SV_DispatchThreadID) {
    if(any(DispatchThreadID >= GetActiveCamera().FilmDimensions)) return;
    float Depth = DepthTexture.Load(uint3(DispatchThreadID, 0)).x;
    if(Depth == 0) {
        // Empty pixel
        return;
    }
    uint4 Visibility = VisibilityTexture.Load(uint3(DispatchThreadID, 0));
    // Decode visibility
    float2 Barycentrics = asfloat(Visibility.wz);
    uint PrimitiveIndex = Visibility.y;
    uint RenderableIndex = Visibility.x & 0xFFFFFF;
    uint DescriptorRank = (Visibility.x >> 24) & 0xFF;

    IntersectionMaterial Intersection = 
        EvaluateStaticMeshRenderableIntersectionMaterial(
            RenderableIndex, 
            DescriptorRank, 
            PrimitiveIndex,
            Barycentrics
        );
    // Write to G-Buffers
    {
        RWAlbedo[DispatchThreadID] = float4(Intersection.Albedo, Intersection.Opacity);
        // Squash normal to [0,1]
        float3 GBufferNormal = (Intersection.Normal.xyz * 0.5f) + 0.5f;
        RWNormal[DispatchThreadID] = float4(GBufferNormal, 1);
        RWEmission[DispatchThreadID] = float4(Intersection.Emission, 1);
        RWMetallicRoughness[DispatchThreadID] = Intersection.MetallicRoughness;
    }
}




struct DrawForwardStaticMeshesVSOut {
    float4 Position : SV_POSITION;
    uint   DescriptorRenderableIndex : TEXCOORD0; // Packed DescriptorIndex (8bits) RenderableIndex (24bits)
    uint   MaterialIndex : TEXCOORD1;
};

DrawForwardStaticMeshesVSOut DrawForwardStaticMeshesVS (DefaultStaticMeshVertex Vertex, uint InstanceIndex : SV_InstanceID) {
    uint2 RenderableIndex_DescriptorIndex = RenderableIndexAndDescriptorIndexBuffer[InstanceIndex];
    uint RenderableIndex = RenderableIndex_DescriptorIndex.x;
    uint DescriptorIndex = RenderableIndex_DescriptorIndex.y;
    float3x4 ToWorldTransform = RenderableTransformBuffer[RenderableIndex];
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    float4 PositionW = mul(View.Camera.WorldToNDC_ReversedZ, float4(WorldPosition, 1));

    DrawForwardStaticMeshesVSOut Output = (DrawForwardStaticMeshesVSOut)0;
    Output.Position = PositionW;
    Output.UV = Vertex.UV;
    Output.DescriptorRenderableIndex = DescriptorIndex << 24 | RenderableIndex;
    uint StaticMeshIndex = RenderableHeaderBuffer[RenderableIndex].StaticMeshIndex;
    StaticMeshHeader StaticMeshHeader = StaticMeshHeaderBuffer[StaticMeshIndex];
    // Compute the global descriptor index
    uint  GlobalDescriptorIndex = StaticMeshHeader.DescriptionOffset + DescriptorIndex;
    uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[GlobalDescriptorIndex];
    Output.MaterialIndex = GeometryMaterialPair.y;
    return Output;
}


struct DrawForwardStaticMeshesPSOut {
    uint4 Visibility : SV_TARGET0; // Forward visibility
    float4 ColorAlpha: SV_TARGET1;
};

DrawForwardStaticMeshesPSOut DrawForwardStaticMeshesPS (
    DrawForwardStaticMeshesVSOut Input, 
    uint PrimitiveIndex : SV_PrimitiveID,
    float2 Barycentrics : SV_Barycentrics
) {
    DrawForwardStaticMeshesPSOut Output = (DrawForwardStaticMeshesPSOut)0;
    Output.Visibility = uint4(
        Input.DescriptorRenderableIndex, 
        PrimitiveIndex,
        asuint(Barycentrics.x),
        asuint(Barycentrics.y)
    );

    // Decode visibility
    IntersectionMaterial Intersection = EvaluateStaticMeshRenderableIntersectionMaterial(
        Input.DescriptorRenderableIndex & 0xFFFFFF, 
        (Input.DescriptorRenderableIndex >> 24) & 0xFF, 
        PrimitiveIndex,
        Barycentrics
    );

    Output.ColorAlpha = float4(Intersection.Albedo, Intersection.Opacity);
    return Output;
}
