#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Math.hlsl"
#include "resources/BindlessTextureResources.hlsl"
#include "shared/SharedStaticMesh.hlsl"

StructuredBuffer<StaticMeshInstanceHeader> RenderableHeaderBuffer;
StructuredBuffer<float3x4>         RenderableTransformBuffer;
StructuredBuffer<float3x3>         RenderableNormalTransformBuffer; // InvTranspose of RenderableTransformBuffer
StructuredBuffer<uint2>            RenderableIndexAndDescriptorIndexBuffer;
StructuredBuffer<uint2>            StaticMeshDescriptionBuffer;

StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<GeometryHeader>   GeometryHeaderBuffer;
StructuredBuffer<MaterialHeader>   MaterialHeaderBuffer;

StructuredBuffer<uint> IndexBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;

SamplerState LinearWrapSampler;
SamplerState PointWrapSampler;


struct DrawDeferredStaticMeshesVSOut {
    float4 Position : SV_POSITION;
    uint   DescriptorRenderableIndex : TEXCOORD0; // Packed DescriptorIndex (8bits) RenderableIndex (24bits)
    float2 UV : TEXCOORD1;
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
    Output.UV = Vertex.UV;
    Output.DescriptorRenderableIndex = DescriptorIndex << 24 | RenderableIndex;
    return Output;
}


struct DrawDeferredStaticMeshesPSOut {
    uint4 Visibility : SV_TARGET0;
};

DrawDeferredStaticMeshesPSOut DrawDeferredStaticMeshesPS (DrawDeferredStaticMeshesVSOut Input, uint PrimitiveIndex : SV_PrimitiveID) {
    DrawDeferredStaticMeshesPSOut Output = (DrawDeferredStaticMeshesPSOut)0;
    Output.Visibility = uint4(
        Input.DescriptorRenderableIndex, 
        PrimitiveIndex,
        asuint(Input.UV.x),
        asuint(Input.UV.y)
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
    float2 UV = asfloat(Visibility.wz);
    uint PrimitiveIndex = Visibility.y;
    uint RenderableIndex = Visibility.x & 0xFFFFFF;
    StaticMeshInstanceHeader InstanceHeader = RenderableHeaderBuffer[RenderableIndex];
    uint StaticMeshIndex = InstanceHeader.StaticMeshIndex;
    StaticMeshHeader StaticMesh = StaticMeshHeaderBuffer[StaticMeshIndex];
    uint DescriptionOffset = StaticMeshHeaderBuffer[StaticMeshIndex].DescriptionOffset;
    uint DescriptorIndex = ((Visibility.x >> 24) & 0xFF) + StaticMesh.DescriptionOffset;
    uint2 Descriptor = StaticMeshDescriptionBuffer[DescriptorIndex];
    uint GeometryIndex = Descriptor.x;
    uint MaterialIndex = Descriptor.y;

    MaterialHeader Material = MaterialHeaderBuffer[MaterialIndex];

    // Spawn G-Buffers
    float4 AlbedoAlpha = float4(Material.Albedo, 1);
    float3 Normal;
    float3 Emission = Material.Emissive;
    float2 MetallicRoughness = float2(Material.Metallic, Material.Roughness);

    bool bPointSampled = Material.Flags & MATERIAL_FLAG_POINT_SAMPLED;
    // Albedo
    if(IsValid(Material.AlbedoMap)) {
        if (bPointSampled) {
            AlbedoAlpha.rgb = GetBindlessSRV(Material.AlbedoMap).Sample(PointWrapSampler, UV).rgb;
        } else {
            AlbedoAlpha.rgb = GetBindlessSRV(Material.AlbedoMap).Sample(LinearWrapSampler, UV).rgb;
        }
    }
    // Normal
    {
        GeometryHeader Geometry = GeometryHeaderBuffer[GeometryIndex];
        uint IndexOffset = Geometry.IndexOffset + PrimitiveIndex * 3;
        uint VertexOffset = Geometry.VertexOffset;
        uint VertexAIndex = VertexOffset + IndexBuffer[IndexOffset + 0];
        uint VertexBIndex = VertexOffset + IndexBuffer[IndexOffset + 1];
        uint VertexCIndex = VertexOffset + IndexBuffer[IndexOffset + 2];
        DefaultStaticMeshVertex VertexA = VertexBuffer[VertexAIndex];
        DefaultStaticMeshVertex VertexB = VertexBuffer[VertexBIndex];
        DefaultStaticMeshVertex VertexC = VertexBuffer[VertexCIndex];
        float3x3 NormalTransform = RenderableNormalTransformBuffer[RenderableIndex];
        // Vertex Normal
        Normal = normalize(InterpolateBarycentrics(
            mul(NormalTransform, VertexA.Normal),
            mul(NormalTransform, VertexB.Normal),
            mul(NormalTransform, VertexC.Normal),
            UV
        ));
        if(IsValid(Material.NormalMap)) {
            // 由于 ddx/ddy 在 Compute Shader 中不可用，我们需要手动重建 TBN 矩阵。
            // 1. 获取三角形的三个顶点在世界空间的位置和 UV。
            //    (顶点法线已经在前面获取过了)
            float3x4 ToWorldTransform = RenderableTransformBuffer[RenderableIndex];
            float3 PosA = TransformPoint(ToWorldTransform, VertexA.Position);
            float3 PosB = TransformPoint(ToWorldTransform, VertexB.Position);
            float3 PosC = TransformPoint(ToWorldTransform, VertexC.Position);
            float2 UV_A = VertexA.UV;
            float2 UV_B = VertexB.UV;
            float2 UV_C = VertexC.UV;

            // 2. 计算世界空间和 UV 空间的边。
            float3 EdgePos1 = PosB - PosA;
            float3 EdgePos2 = PosC - PosA;
            float2 EdgeUV1 = UV_B - UV_A;
            float2 EdgeUV2 = UV_C - UV_A;

            // 3. 计算 TBN 矩阵。
            //    参考 http://www.opengl-tutorial.org/intermediate-tutorials/tutorial-13-normal-mapping/
            float r = 1.0f / (EdgeUV1.x * EdgeUV2.y - EdgeUV1.y * EdgeUV2.x);
            float3 Tangent   = normalize((EdgePos1 * EdgeUV2.y - EdgePos2 * EdgeUV1.y) * r);
            float3 Bitangent = normalize((EdgePos2 * EdgeUV1.x - EdgePos1 * EdgeUV2.x) * r);
            
            // 4. 修正切线空间，使其与插值后的法线正交 (Gram-Schmidt process)。
            Normal = normalize(mul(NormalTransform, Normal)); // 确保法线已经被正确变换
            Tangent = normalize(Tangent - dot(Tangent, Normal) * Normal);
            
            // 5. 计算 Bitangent 的handedness，并修正 Bitangent。
            float handedness = dot(cross(Normal, Tangent), Bitangent) < 0.0f ? -1.0f : 1.0f;
            Bitangent = cross(Normal, Tangent) * handedness;

            // 6. 采样法线贴图并变换法线。
            float3 NormalMapSample;
            if (bPointSampled) {
                NormalMapSample = GetBindlessSRV(Material.NormalMap).Sample(PointWrapSampler, UV).xyz * 2.0f - 1.0f;
            } else {
                NormalMapSample = GetBindlessSRV(Material.NormalMap).Sample(LinearWrapSampler, UV).xyz * 2.0f - 1.0f;
            }
            
            Normal = normalize(
                NormalMapSample.x * Tangent +
                NormalMapSample.y * Bitangent +
                NormalMapSample.z * Normal
            );
        }
    }
    // Emission
    if(IsValid(Material.EmissiveMap)) {
        float4 EmissionA;
        if (bPointSampled) {
            EmissionA = GetBindlessSRV(Material.EmissiveMap).Sample(PointWrapSampler, UV);
        } else {
            EmissionA = GetBindlessSRV(Material.EmissiveMap).Sample(LinearWrapSampler, UV);
        }
        // For A channel, we assume it's a exponential multiplier (2 base)
        Emission = EmissionA.rgb * pow(2.0f, EmissionA.a * 255);
    }
    if(IsValid(Material.MetallicRoughnessMap)) {
        float2 MetallicRoughness;
        if (bPointSampled) {
            MetallicRoughness = GetBindlessSRV(Material.MetallicRoughnessMap).Sample(PointWrapSampler, UV).xy;
        } else {
            MetallicRoughness = GetBindlessSRV(Material.MetallicRoughnessMap).Sample(LinearWrapSampler, UV).xy;
        }
    }
    // Squash normal to [0,1]
    Normal.xyz = (Normal.xyz * 0.5f) + 0.5f;
    // Write to G-Buffers
    {
        RWAlbedo[DispatchThreadID] = float4(AlbedoAlpha.rgb, 1);
        RWNormal[DispatchThreadID] = float4(Normal, 1);
        RWEmission[DispatchThreadID] = float4(Emission, 1);
        RWMetallicRoughness[DispatchThreadID] = MetallicRoughness;
    }
}




struct DrawForwardStaticMeshesVSOut {
    float4 Position : SV_POSITION;
    uint   DescriptorRenderableIndex : TEXCOORD0; // Packed DescriptorIndex (8bits) RenderableIndex (24bits)
    float2 UV : TEXCOORD1;
    uint   MaterialIndex : TEXCOORD2;
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

DrawForwardStaticMeshesPSOut DrawForwardStaticMeshesPS (DrawForwardStaticMeshesVSOut Input, uint PrimitiveIndex : SV_PrimitiveID) {
    DrawForwardStaticMeshesPSOut Output = (DrawForwardStaticMeshesPSOut)0;
    Output.Visibility = uint4(
        Input.DescriptorRenderableIndex, 
        PrimitiveIndex,
        asuint(Input.UV.x),
        asuint(Input.UV.y)
    );
    MaterialHeader Material = MaterialHeaderBuffer[Input.MaterialIndex];
    float4 ColorAlpha = float4(Material.Albedo, 1);

    bool bPointSampled = Material.Flags & MATERIAL_FLAG_POINT_SAMPLED;
    // Albedo
    if(IsValid(Material.AlbedoMap)) {
        if (bPointSampled) {
            ColorAlpha *= GetBindlessSRV(Material.AlbedoMap).Sample(
                PointWrapSampler, Input.UV
            );
        } else {
            ColorAlpha *= GetBindlessSRV(Material.AlbedoMap).Sample(
                LinearWrapSampler, Input.UV
            );
        }
    }
    Output.ColorAlpha = ColorAlpha;
    return Output;
}
