#ifndef LIGHT_EVALUATION_HLSL
#define LIGHT_EVALUATION_HLSL
#include "../headers/Transform.hlsl"
#include "../headers/Conventions.hlsl"
#include "../headers/Packing.hlsl"
#include "../headers/Light.hlsl"
#include "CommonSamplerResources.hlsl"
#include "BindlessTextureResources.hlsl"
#include "RenderableResources.hlsl"
#include "StaticMeshResources.hlsl"
#include "GeometryResources.hlsl"
#include "MaterialResources.hlsl"

EvaluatedAreaLight EvaluateLight(AreaLight Light, out bool bActive) {
    EvaluatedAreaLight EvaluatedLightData = (EvaluatedAreaLight)0;
    // Extract light data
    uint RenderableIndex = Light.RenderableIndex;
    uint StaticMeshDescriptionOffset = Light.StaticMeshDescriptionIndex;
    StaticMeshInstanceHeader InstanceHeader = GetStaticMeshInstanceHeader(RenderableHeaderBuffer[RenderableIndex]);
    if(InstanceHeader.Flags & RENDERABLE_VISIBLE_FLAG_BIT) {
        bActive = true;
    } else {
        bActive = false;
    }
    StaticMeshHeader StaticMesh = StaticMeshHeaderBuffer[InstanceHeader.StaticMeshIndex];
    uint DescriptionIndex = StaticMesh.DescriptionOffset + StaticMeshDescriptionOffset;
    uint2 GeometryMaterial = StaticMeshDescriptionBuffer[DescriptionIndex];
    uint PrimitiveIndex = Light.PrimitiveIndex;
    uint LightFlags = Light.Flags;
    // TODO monitor light changes
    float3x4 ToWorldTransform = RenderableTransformBuffer[RenderableIndex];
    GeometryHeader Geometry = GeometryHeaderBuffer[GeometryMaterial.x];
    uint VertexOffset = Geometry.VertexOffset;
    uint IndexOffset = Geometry.IndexOffset + PrimitiveIndex * 3;
    uint I0 = VertexOffset + IndexBuffer[IndexOffset];
    uint I1 = VertexOffset + IndexBuffer[IndexOffset + 1];
    uint I2 = VertexOffset + IndexBuffer[IndexOffset + 2];
    EvaluatedLightData.V0 = TransformPoint(ToWorldTransform, VertexBuffer[I0].Position);
    EvaluatedLightData.V1 = TransformPoint(ToWorldTransform, VertexBuffer[I1].Position);
    EvaluatedLightData.V2 = TransformPoint(ToWorldTransform, VertexBuffer[I2].Position);
    // Estimate emission
    MaterialHeader Material = MaterialHeaderBuffer[GeometryMaterial.y];
    EvaluatedLightData.Emission = Material.Emissive;
    if (IsValid(Material.EmissiveMap)) {
        // Sample the center pixel for approximation
        // TODO: Sample a qualified LOD
        float2 UV0 = VertexBuffer[I0].UV;
        float2 UV1 = VertexBuffer[I1].UV;
        float2 UV2 = VertexBuffer[I2].UV;
        float2 UV = (UV0 + UV1 + UV2) * (1.f / 3.f);
        EvaluatedLightData.EstimatedAverageEmission = GetBindlessSRV(Material.EmissiveMap).SampleLevel(LinearWrapSampler, UV, 0).rgb;
    }
    EvaluatedLightData.EmissionTextureIndex = Material.EmissiveMap;
    EvaluatedLightData.EstimatedAverageEmission += Material.Emissive;
    return EvaluatedLightData;
}

#endif