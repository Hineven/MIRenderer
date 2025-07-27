#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/BindlessTextures.hlsl"

StructuredBuffer<RenderableHeader> RenderableHeaders;
StructuredBuffer<float3x4>         RenderableTransforms;
StructuredBuffer<float3x3>         RenderableNormalTransforms; // InvTranspose of RenderableTransforms
StructuredBuffer<uint2>            RenderableIndexAndMaterialIndex;
StructuredBuffer<MaterialHeader>   MaterialHeaders;

SamplerState Sampler;
SamplerState PointSampler;

struct VS_Output {
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    float3 Normal : NORMAL;
    uint   MaterialIndex : TEXCOORD1;
    float3 WorldPosition : TEXCOORD2; // For tangent computation in PS
};

VS_Output VS_Main (DefaultStaticMeshVertex Vertex, uint InstanceIndex : SV_InstanceID) {
    uint RenderableIndex = RenderableIndexAndMaterialIndex[InstanceIndex].x;
    float3x4 ToWorldTransform = RenderableTransforms[RenderableIndex];
    float3x3 ToWorldNormalTransform = RenderableNormalTransforms[RenderableIndex];
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    float3 WorldNormal   = mul(ToWorldNormalTransform, Vertex.Normal);
    float4 PositionW = mul(View.Camera.WorldToNDC_ReversedZ, float4(WorldPosition, 1));

    VS_Output Output = (VS_Output)0;
    Output.Position = PositionW;
    Output.UV = Vertex.UV;
    Output.Normal = WorldNormal;
    Output.MaterialIndex = RenderableIndexAndMaterialIndex[InstanceIndex].y;
    Output.WorldPosition = WorldPosition;
    return Output;
}

struct PS_Output {
    float4 AlbedoAlpha : SV_TARGET0;
    float4 Normal : SV_TARGET1;
    float4 MetallicRoughness : SV_TARGET2;
};

PS_Output PS_Main (VS_Output Input) {
    MaterialHeader Material = MaterialHeaders[Input.MaterialIndex];
    bool bPointSampled = Material.Flags & MATERIAL_FLAG_POINT_SAMPLED;
    PS_Output Output = (PS_Output)0;
    Output.AlbedoAlpha = float4(Material.Albedo, 1);
    Output.Normal = float4(Input.Normal, 0);
    Output.MetallicRoughness = float4(Material.Metallic, Material.Roughness, 0, 1);
    if(IsValid(Material.AlbedoMap)) {
        if (bPointSampled) {
            Output.AlbedoAlpha.rgb = GetBindlessSRV(Material.AlbedoMap).Sample(PointSampler, Input.UV).rgb;
        } else {
            Output.AlbedoAlpha.rgb = GetBindlessSRV(Material.AlbedoMap).Sample(Sampler, Input.UV).rgb;
        }
    }
    if(IsValid(Material.NormalMap)) {
        // online tbn construction
        float3 dpdx = ddx(Input.WorldPosition);
        float3 dpdy = ddy(Input.WorldPosition);
        float2 duvdx = ddx(Input.UV);
        float2 duvdy = ddy(Input.UV);
        float3 Tangent = normalize(dpdx * duvdy.y - dpdy * duvdx.y);
        float3 Normal = normalize(Input.Normal);
        float3 Bitangent = cross(Normal, Tangent);

        float3 NormalMapSample;
        if (bPointSampled) {
            NormalMapSample = GetBindlessSRV(Material.NormalMap).Sample(PointSampler, Input.UV).xyz * 2 - 1;
        } else {
            NormalMapSample = GetBindlessSRV(Material.NormalMap).Sample(Sampler, Input.UV).xyz * 2 - 1;
        }
        Output.Normal.xyz = normalize(
            NormalMapSample.x * Tangent +
            NormalMapSample.y * Bitangent +
            NormalMapSample.z * Normal
        );
    }
    if(IsValid(Material.MetallicRoughnessMap)) {
        float2 MetallicRoughness;
        if (bPointSampled) {
            MetallicRoughness = GetBindlessSRV(Material.MetallicRoughnessMap).Sample(PointSampler, Input.UV).xy;
        } else {
            MetallicRoughness = GetBindlessSRV(Material.MetallicRoughnessMap).Sample(Sampler, Input.UV).xy;
        }
        Output.MetallicRoughness = float4(MetallicRoughness.x, MetallicRoughness.y, 0, 1);
    }
    return Output;
}
