#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Math.hlsl"
#include "headers/Random.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"

StructuredBuffer<uint2>            RenderableIndexAndDescriptorIndexBuffer;

struct DrawDeferredStaticMeshesVSOut {
    float4 Position : SV_POSITION;
    uint   DescriptorRenderableIndex : TEXCOORD0; // Packed DescriptorIndex (8bits) RenderableIndex (24bits)
    uint   MaterialIndex : TEXCOORD1;
    float2 UV : TEXCOORD2;
};

// TODO provide an optimized path for opaque materials
DrawDeferredStaticMeshesVSOut DrawDeferredStaticMeshesVS (
    DefaultStaticMeshVertex Vertex,
    uint InstanceIndex : SV_InstanceID,
    uint BaseInstance : SV_StartInstanceLocation
) {
    // InstanceIndex += BaseInstance;
    uint2 RenderableIndex_DescriptorIndex = RenderableIndexAndDescriptorIndexBuffer[InstanceIndex];
    uint RenderableIndex = RenderableIndex_DescriptorIndex.x;
    uint DescriptorIndex = RenderableIndex_DescriptorIndex.y;
    float3x4 ToWorldTransform = RenderableTransformBuffer[RenderableIndex];
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    float4 PositionW = mul(View.Camera.WorldToNDC_ReversedZ, float4(WorldPosition, 1));
    
    uint StaticMeshIndex = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[RenderableIndex]).StaticMeshIndex;
    StaticMeshHeader StaticMeshHeader = StaticMeshHeaderBuffer[StaticMeshIndex];
    // Compute the global descriptor index
    uint  GlobalDescriptorIndex = StaticMeshHeader.DescriptionOffset + DescriptorIndex;
    uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[GlobalDescriptorIndex];

    DrawDeferredStaticMeshesVSOut Output = (DrawDeferredStaticMeshesVSOut)0;
    Output.Position = PositionW;
    Output.DescriptorRenderableIndex = DescriptorIndex << 24 | RenderableIndex;
    Output.MaterialIndex = GeometryMaterialPair.y;
    // Here we use the faster path to interpolate UVs rather than decoding full visibility in fragment shader.
    Output.UV = Vertex.UV;
    return Output;
}


struct DrawDeferredStaticMeshesPSOut {
    uint4 Visibility : SV_TARGET0;
};

DrawDeferredStaticMeshesPSOut DrawDeferredStaticMeshesPS (
    DrawDeferredStaticMeshesVSOut Input,
    uint PrimitiveIndex : SV_PrimitiveID,
    float3 Barycentrics : SV_BaryCentrics
) {
    DrawDeferredStaticMeshesPSOut Output = (DrawDeferredStaticMeshesPSOut)0;
    MaterialHeader Material = MaterialHeaderBuffer[Input.MaterialIndex];
    if(0 == (Material.Flags & MATERIAL_FLAG_OPAQUE)) {
        float4 ColorOpacity = float4(Material.Albedo, 1);
        if(IsValid(Material.AlbedoMap)) {
            ColorOpacity = GetBindlessSRV(Material.AlbedoMap).Sample(LinearWrapSampler, Input.UV);
        }
        // stochastic alpha test
        float MinAlpha = 0.3f;
        float MaxAlpha = 0.7f;
        CameraParameters C = GetActiveCamera();
        float2 NoiseUV = float2(Input.UV.x * C.FilmAspectRatioAndInvAspectRatio.x, Input.UV.y);
        float Threshold = lerp(MinAlpha, MaxAlpha, InterleavedGradientNoise(NoiseUV * 259, GetViewFrameIndex()));
        if(ColorOpacity.a < Threshold) {
            // Discard the pixel
            discard;
        }
    }
    Output.Visibility = uint4(
        Input.DescriptorRenderableIndex, 
        PrimitiveIndex,
        asuint(Barycentrics.y), // Keep the latter 2 floats
        asuint(Barycentrics.z)
    );
    return Output;
}

[[vk::image_format("rgba8")]]
RWTexture2D<float4> RWAlbedo;
[[vk::image_format("rgba8")]]
RWTexture2D<float4> RWNormal;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWEmission;
[[vk::image_format("rg8")]]
RWTexture2D<float2> RWMetallicRoughness;
Texture2D<uint4> VisibilityTexture;
Texture2D<float> DepthTexture;

[numthreads(1, 1, 1)]
void Test() {
    RWAlbedo[uint2(0,0)] = float4(1,0,0,1);
}

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
    float2 Barycentrics = asfloat(Visibility.zw);
    uint PrimitiveIndex = Visibility.y;
    uint RenderableIndex = Visibility.x & 0xFFFFFF;
    uint DescriptorRank = (Visibility.x >> 24) & 0xFF;

    IntersectionMaterial Intersection = 
        EvaluateStaticMeshRenderableIntersectionMaterial(
            RenderableIndex, 
            DescriptorRank, 
            PrimitiveIndex,
            Barycentrics,
            // TODO: Get a proper LOD
            0
        );
        
    // Write to G-Buffers
    {
        // 25.10.21: Alpha should always be 1.
        RWAlbedo[DispatchThreadID] = float4(Intersection.Albedo, 1.f);//Intersection.Opacity);
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
    float2 UV : TEXCOORD2;
};

DrawForwardStaticMeshesVSOut DrawForwardStaticMeshesVS (
    DefaultStaticMeshVertex Vertex,
    uint InstanceIndex : SV_InstanceID,
    uint BaseInstance : SV_StartInstanceLocation
) {
    // InstanceIndex += BaseInstance;
    uint2 RenderableIndex_DescriptorIndex = RenderableIndexAndDescriptorIndexBuffer[InstanceIndex];
    uint RenderableIndex = RenderableIndex_DescriptorIndex.x;
    uint DescriptorIndex = RenderableIndex_DescriptorIndex.y;
    float3x4 ToWorldTransform = RenderableTransformBuffer[RenderableIndex];
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    float4 PositionW = mul(View.Camera.WorldToNDC_ReversedZ, float4(WorldPosition, 1));

    DrawForwardStaticMeshesVSOut Output = (DrawForwardStaticMeshesVSOut)0;
    Output.Position = PositionW;
    Output.DescriptorRenderableIndex = DescriptorIndex << 24 | RenderableIndex;
    uint StaticMeshIndex = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[RenderableIndex]).StaticMeshIndex;
    StaticMeshHeader StaticMeshHeader = StaticMeshHeaderBuffer[StaticMeshIndex];
    // Compute the global descriptor index
    uint  GlobalDescriptorIndex = StaticMeshHeader.DescriptionOffset + DescriptorIndex;
    uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[GlobalDescriptorIndex];
    Output.MaterialIndex = GeometryMaterialPair.y;

    // Here we use the faster path to interpolate UVs rather than decoding full visibility in fragment shader.
    Output.UV = Vertex.UV;
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

    MaterialHeader Material = MaterialHeaderBuffer[Input.MaterialIndex];
    float4 ColorOpacity = float4(Material.Albedo, 1);
    if(IsValid(Material.AlbedoMap)) {
        ColorOpacity = GetBindlessSRV(Material.AlbedoMap).Sample(LinearWrapSampler, Input.UV);
    }

    Output.ColorAlpha = ColorOpacity;
    return Output;
}
