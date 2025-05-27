#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/BindlessTextures.hlsl"

StructuredBuffer<RenderableHeader> RenderableHeaders;
StructuredBuffer<float3x4>         RenderableTransforms;
StructuredBuffer<uint2>            RenderableIndexAndMaterialIndex;
StructuredBuffer<MaterialHeader>   MaterialHeaders;

SamplerState Sampler;

struct VS_Output {
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
    float3 Normal : NORMAL;
    uint   MaterialIndex : TEXCOORD1;
};

VS_Output VS_Main (DefaultStaticMeshVertex Vertex, uint InstanceIndex : SV_InstanceIndex) : SV_POSITION {
    uint RenderableIndex = RenderableIndexAndMaterialIndex[InstanceIndex].x;
    float3x4 ToWorldTransform = RenderableTransforms[RenderableIndex];
    float3x4 ToWorldTransformInverse = transpose(ToWorldTransform);
    float3 WorldPosition = mul(ToWorldTransform, float4(Vertex.Position, 1));
    // float3 WorldNormal   = mul()
    float4 PositionW = mul(View.Camera.WorldToNDC, float4(WorldPosition, 1));

    VS_Output Output = (VS_Output)0;
    Output.Position = PositionW;
    Output.UV = Vertex.UV;
    Output.Normal = float3(0, 0, 1);//mul(ToWorldTransform, float4(Vertex.Normal, 0)).xyz;
    Output.MaterialIndex = RenderableIndexAndMaterialIndex[InstanceIndex].y;
    return Output;
}

struct PS_Output {
    float4 AlbedoAlpha : SV_TARGET;
    float4 Normal : SV_TARGET1;
    float2 MetallicRoughness : SV_TARGET2;
};

PS_Output PS_Main (VS_Output Input) : SV_TARGET {
    MaterialHeader Material = MaterialHeaders[Input.MaterialIndex];
    PS_Output Output = (PS_Output)0;
    Output.AlbedoAlpha = float4(Material.Albedo, 1);
    if(IsValid(Material.AlbedoMap)) {
        Output.AlbedoAlpha.rgb = GetBindlessSRV(Material.AlbedoMap).Sample(Sampler, Input.UV).rgb;
    }
    // if(IsValid(Material.NormalMap)) {
    //     Output.Normal.xyz = (Material.NormalMap).Sample(Sampler, Input.UV).xyz * 2 - 1;
    // }
    if(IsValid(Material.MetallicRoughnessMap)) {
        float2 MetallicRoughness = GetBindlessSRV(Material.MetallicRoughnessMap).Sample(Sampler, Input.UV).xy;
        Output.MetallicRoughness = float2(MetallicRoughness.x, MetallicRoughness.y);
    }
    Output.Normal = float4(Input.Normal, 0);
    Output.MetallicRoughness = float2(Material.Metallic, Material.Roughness);
    return Output;
}

