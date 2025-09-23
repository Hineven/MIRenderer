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
#include "../resources/MaterialResources.hlsl"

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

// Use an extra factor on the estimation of light contribution.
#define LIGHT_PROJECTION_ESTIMATION

// A coarse estimtion used for light -> point contribution
float EstimateLightContribution(PrecomputedLight L, float3 Position, float3 Normal, bool bVolume = false) {
    if(bVolume) {
        // Position is from a sample of volume scattering media. Normal is the view direction.
        float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3.0f;
        float3 ToLightCenter = LightCenter - Position;
        float DistanceSq = dot(ToLightCenter, ToLightCenter);

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
        // Take into account the cosine factor how much the sampled point is facing towards the light
        // Regarding the nature of volume scattering, lights that the sample is not facing towards will still have a lower
        // effect on the sample. So a constant bias of 1.5 and a scaling factor of 0.4 are applied.
        float CosineFactor = saturate(CosineBias + (1.5f + dot(Normal, ToLightDirection)) * 0.4f);
        // TODO take account of different parameterizations of HG phase function

        // Take account for how well is the light facing the shading point
        float LightFacingCosineFactor = saturate(-dot(ToLightDirection, L.Normal));
        // In case the light is close to the shading point, reduce the effect from LightFacingCosineFactor
        if(LightFacingCosineFactor > 0 && DistanceSq < 1.5f * MaxLightRadiusSq)
            LightFacingCosineFactor = lerp(1, LightFacingCosineFactor, DistanceSq / max(1.5f * MaxLightRadiusSq, 1e-4f));
#ifndef LIGHT_PROJECTION_ESTIMATION
        LightFacingCosineFactor = 1;
#endif

        float SolidAngle = LightArea * LightFacingCosineFactor / (DistanceSq + LightArea);
        return L.Intensity * SolidAngle * CosineFactor / PI;
    } else {
        float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3.0f;
        float3 ToLightCenter = LightCenter - Position;
        float DistanceSq = dot(ToLightCenter, ToLightCenter);

        
        float3 ToLightDirection = normalize(ToLightCenter);

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
        // Take into account the cosine factor how much the sampled point is facing towards the light
        float CosineFactor = saturate(CosineBias + dot(Normal, ToLightCenter));

        // Take account for how well is the light facing the shading point
        float LightFacingCosineFactor = saturate(-dot(ToLightDirection, L.Normal));
        // In case the light is close to the shading point, reduce the effect from LightFacingCosineFactor
        if(LightFacingCosineFactor > 0 && DistanceSq < 1.5f * MaxLightRadiusSq)
            LightFacingCosineFactor = lerp(1, LightFacingCosineFactor, DistanceSq / max(1.5f * MaxLightRadiusSq, 1e-4f));

#ifndef LIGHT_PROJECTION_ESTIMATION
        LightFacingCosineFactor = 1;
#endif

        float SolidAngle = LightArea * LightFacingCosineFactor / (DistanceSq + LightArea);
        return L.Intensity * SolidAngle * CosineFactor / PI;
    }
}

#undef LIGHT_PROJECTION_ESTIMATION

#endif