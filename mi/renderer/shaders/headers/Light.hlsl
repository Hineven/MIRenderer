#ifndef LIGHT_HLSL
#define LIGHT_HLSL
#include "Transform.hlsl"
#include "Conventions.hlsl"
#include "Packing.hlsl"

#include "../shared/SharedLight.hlsl"
#include "../shared/SharedStaticMesh.hlsl"
#include "../shared/SharedMaterial.hlsl"
#include "../resources/CommonSamplerResources.hlsl"
#include "../resources/BindlessTextureResources.hlsl"
#include "../resources/GeometryResources.hlsl"

PackedPrecomputedLight PackPrecomputedLight(PrecomputedLight L) {
    PackedPrecomputedLight P = (PackedPrecomputedLight)0;
    P.V0 = L.V0;
    P.V1 = L.V1;
    P.V2 = L.V2;
    P.Normal = PackNormal(L.Normal);
    P.Intensity = L.Intensity;
    return P;
}

PrecomputedLight UnpackPrecomputedLight(PackedPrecomputedLight P) {
    PrecomputedLight L = (PrecomputedLight)0;
    L.V0 = P.V0;
    L.V1 = P.V1;
    L.V2 = P.V2;
    L.Normal = UnpackNormal(P.Normal);
    L.Intensity = P.Intensity;
    return L;
}

struct EvaluatedLight {
    float3 V0, V1, V2;
    float2 UV0, UV1, UV2;
    uint EmissionTextureIndex;
    float3 Emission;
    float3 EstimatedAverageEmission;
};

EvaluatedLight EvaluateLight(AreaLight Light) {
    EvaluatedLight EvaluatedLightData = (EvaluatedLight)0;
    // Extract light data
    uint RenderableIndex = Light.RenderableIndex;
    uint StaticMeshDescriptionOffset = Light.StaticMeshDescriptionIndex;
    StaticMeshHeader StaticMesh = StaticMeshHeaderBuffer[RenderableIndex];
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