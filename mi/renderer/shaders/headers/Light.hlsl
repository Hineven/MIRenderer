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

// A coarse estimtion used for light -> point contribution
float EstimateLightContribution(PrecomputedLight L, float3 Position, float3 Normal, bool bVolume = false) {
    if(bVolume) {
        // Position is from a sample of volume scattering media. Normal is the view direction.
        float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3.0f;
        float3 ToLightCenter = LightCenter - Position;
        float DistanceSq = dot(ToLightCenter, ToLightCenter);

        // The the light is not facing the sampled position. Cull it out.
        if (dot(ToLightCenter, L.Normal) >= 0.0f) {
            return 0.0f;
        }
        float3 ToLightDirection = normalize(ToLightCenter);

        float LightArea = length(cross(L.V1 - L.V0, L.V2 - L.V0)) * 0.5f;
        float3 CenterToV0 = L.V0 - LightCenter;
        float3 CenterToV1 = L.V1 - LightCenter;
        float3 CenterToV2 = L.V2 - LightCenter;
        float3 LightVertexDistancesSq = float3(
            dot(CenterToV0, CenterToV0),
            dot(CenterToV1, CenterToV1),
            dot(CenterToV2, CenterToV2)
        );
        float MaxLightRadiusSq = max(LightVertexDistancesSq.x, max(LightVertexDistancesSq.y, LightVertexDistancesSq.z));

        float CosineBias = 0;
        {
            // The light is big & close enough (distance < 2 * max light radius), reduce the effect from CosineFactor
            CosineBias = 1.f - saturate(DistanceSq / (2 * MaxLightRadiusSq));
        }
        // Regarding the nature of volume scattering, lights that the sample is not facing towards will still have a lower
        // effect on the sample. So a constant bias of 1.5 and a scaling factor of 0.4 are applied.
        float CosineFactor = saturate(CosineBias + (1.5f + dot(Normal, ToLightDirection)) * 0.4f);
        // TODO take account of different parameterizations of HG phase function

        float SolidAngle = LightArea / (DistanceSq + LightArea);
        return L.Intensity * SolidAngle * CosineFactor / PI;
    } else {
        float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3.0f;
        float3 ToLightCenter = LightCenter - Position;
        float DistanceSq = dot(ToLightCenter, ToLightCenter);

        float3 ToV0 = L.V0 - Position;
        float3 ToV1 = L.V1 - Position;
        float3 ToV2 = L.V2 - Position;

        // The the light is not facing the sampled position. Cull it out.
        if (dot(ToLightCenter, L.Normal) >= 0.0f) {
            return 0.0f;
        }

        float k0 = saturate(dot(Normal, ToV0));
        float k1 = saturate(dot(Normal, ToV1));
        float k2 = saturate(dot(Normal, ToV2));
        // the sampled surface is not facing the light. Cull it out.
        if (all(float3(k0, k1, k2) <= 0.0f)) {
            return 0.0f;
        }

        float LightArea = length(cross(L.V1 - L.V0, L.V2 - L.V0)) * 0.5f;
        float3 CenterToV0 = L.V0 - LightCenter;
        float3 CenterToV1 = L.V1 - LightCenter;
        float3 CenterToV2 = L.V2 - LightCenter;
        float3 LightVertexDistancesSq = float3(
            dot(CenterToV0, CenterToV0),
            dot(CenterToV1, CenterToV1),
            dot(CenterToV2, CenterToV2)
        );
        float MaxLightRadiusSq = max(LightVertexDistancesSq.x, max(LightVertexDistancesSq.y, LightVertexDistancesSq.z));

        float CosineBias = 0;
        {
            // The light is big & close enough (distance < 2 * max light radius), reduce the effect from CosineFactor
            CosineBias = 1.f - saturate(DistanceSq / (2 * MaxLightRadiusSq));
        }

        float CosineFactor = saturate(CosineBias + dot(Normal, ToLightCenter));

        float SolidAngle = LightArea / (DistanceSq + LightArea);
        return L.Intensity * SolidAngle * CosineFactor / PI;
    }
}

#endif