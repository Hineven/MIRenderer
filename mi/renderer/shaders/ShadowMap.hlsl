#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "headers/Camera.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Math.hlsl"
#include "resources/IntersectionEvaluationResources.hlsl"

StructuredBuffer<uint2> RenderableIndexAndDescriptorIndexBuffer;

ConstantBuffer<DirectionalLightForShadowMap> LightView;

struct Shadow_VS_Out
{
    float4 Position : SV_POSITION;
    uint MaterialIndex : TEXCOORD0;
    float2 UV : TEXCOORD1;
};

Shadow_VS_Out Shadow_VS_Main(
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
    float4 PositionL = mul(LightView.LightWorldToNDC, float4(WorldPosition, 1));
    
    uint StaticMeshIndex = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[RenderableIndex]).StaticMeshIndex;
    StaticMeshHeader StaticMeshHeader = StaticMeshHeaderBuffer[StaticMeshIndex];
    uint GlobalDescriptorIndex = StaticMeshHeader.DescriptionOffset + DescriptorIndex;
    uint2 GeometryMaterialPair = StaticMeshDescriptionBuffer[GlobalDescriptorIndex];
    
    Shadow_VS_Out Output = (Shadow_VS_Out) 0;
    Output.Position = PositionL;
    Output.MaterialIndex = GeometryMaterialPair.y;
    Output.UV = Vertex.UV;
    return Output;
}

struct Shadow_PS_Out
{
    float4 Moments : SV_TARGET0;
};

Shadow_PS_Out Shadow_PS_Main(Shadow_VS_Out Input)
{
    
    MaterialHeader Material = MaterialHeaderBuffer[Input.MaterialIndex];
    float Opacity = 1.0f;
    if (IsValid(Material.AlbedoMap)){
        Opacity = GetBindlessSRV(Material.AlbedoMap).Sample(LinearWrapSampler, Input.UV).w;
    }
    if (Opacity < 0.5f){
        discard;
    }
    
    Shadow_PS_Out Output = (Shadow_PS_Out) 0;
    Output.Moments = float4(Input.Position.z, Input.Position.z * Input.Position.z, 0, 1);
    
    return Output;
}

