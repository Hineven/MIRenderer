#ifndef LIGHT_CLUSTER_HIERARCHY_RESOURCES_HLSL
#define LIGHT_CLUSTER_HIERARCHY_RESOURCES_HLSL

#include "../resources/LightEvaluation.hlsl"
#include "../shared/SharedLightClusterHierarchy.hlsl"

StructuredBuffer<MeshLightTriangle> LCH_MeshLightTriangleBuffer;
StructuredBuffer<MeshLightTriangleHash> LCH_MeshLightTriangleHashBuffer;
StructuredBuffer<MeshLightTriangleBakedData> LCH_MeshLightTriangleBakedDataBuffer;
StructuredBuffer<MeshLightClusterHeader> LCH_MeshLightClusterHeaderBuffer;
StructuredBuffer<MeshLightClusterNode> LCH_MeshLightClusterNodeBuffer;

StructuredBuffer<MeshLight> LCH_MeshLightBuffer;
StructuredBuffer<MeshLightLevelHeader> LCH_MeshLightLevelHeaderBuffer;

StructuredBuffer<MeshLightInstance> LCH_MeshLightInstanceBuffer;

// Nodes for each instance cluster. Updated per frame.
StructuredBuffer<MeshLightInstanceClusterHeader> LCH_MeshLightInstanceClusterHeaderBuffer;
RWStructuredBuffer<MeshLightInstanceClusterHeader> LCH_RWMeshLightInstanceClusterHeaderBuffer;
StructuredBuffer<MeshLightInstanceClusterNode> LCH_MeshLightInstanceClusterNodeBuffer;
RWStructuredBuffer<MeshLightInstanceClusterNode> LCH_RWMeshLightInstanceClusterNodeBuffer;
StructuredBuffer<MeshLightInstanceTriangle> LCH_MeshLightInstanceTriangleBuffer;
RWStructuredBuffer<MeshLightInstanceTriangle> LCH_RWMeshLightInstanceTriangleBuffer;


static const float LCH_PI = 3.14159265358979323846f;

float LightGrid_EstimateLightGridPerceptualContribution_ClusterNormalVarianceToLerpFactor(float v) {
    const float kVar = 0.30666667f;
    const float kT   = 0.73000000f;
    const float v1   = 0.33333333f; // uniform sphere target
    if (v <= 0.f) return 0.f;
    if (v >= v1) return 1.f;
    if (v <= kVar) return kT * v / kVar;
    return kT + (1.f - kT) * (v - kVar) / (v1 - kVar);
}


// A coarse estimtion used for light grid injection
// Estimate Light -> grid contribution
float LightGrid_EstimateLightGridPerceptualContribution(MeshLightInstanceTriangle L, float3 GridMin, float GridSize) {
    // Estimate the contribution from the area light using appriximated solid angle
    // Here, L.Intensity is the luminance of the light x the area of the light

    // Calculate the distance from the light to the grid
    float3 GridCenter = GridMin + GridSize * 0.5f;
    float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3;
    float3 LightToGrid = GridCenter - LightCenter;
    float Distance = length(LightToGrid);
    float3 LightToGridDirection;
    float3 LightNormal = normalize(cross(L.V1 - L.V0, L.V2 - L.V0));
    {
        // Offset the grid center when computing Light-Grid direction for conservative estimation
        float3 LightToGridDirectionUnnormalized = LightToGrid + (LightNormal * sqrt(3.f) * 0.6f * GridSize);
        float  Len = length(LightToGridDirectionUnnormalized);
        if(Len > 1e-6f) {
            LightToGridDirection = LightToGridDirectionUnnormalized / Len;
        } else {
            // The light is very close to the grid center. Fallback to light normal
            LightToGridDirection = LightNormal;
        }
    }
    float LightFacingCosineFactor = saturate(dot(LightNormal, LightToGridDirection));

    float LightArea = length(cross(L.V1 - L.V0, L.V2 - L.V0)) * 0.5f;
    float3 CenterToV0 = L.V0 - LightCenter;
    float3 CenterToV1 = L.V1 - LightCenter;
    float3 CenterToV2 = L.V2 - LightCenter;
    float3 LightVertexDistancesSq = float3(
        dot(CenterToV0, CenterToV0),
        dot(CenterToV1, CenterToV1),
        dot(CenterToV2, CenterToV2)
    );
    float MaxLightRadius = sqrt(max(LightVertexDistancesSq.x, max(LightVertexDistancesSq.y, LightVertexDistancesSq.z)));
    float GridBoundingRadius = GridSize * sqrt(3.f) * 0.5f;

    float EffectiveDistance = max(Distance - (MaxLightRadius + GridBoundingRadius), 1e-3f);
    float EffectiveDistanceSq = EffectiveDistance * EffectiveDistance;
    float SolidAngleNoArea = LightFacingCosineFactor / max(EffectiveDistanceSq + LightArea / PI, 1e-6f);

    // It is the same as L.AvgIntensity * SolidAngle.
    return L.Intensity * SolidAngleNoArea;
}


// A coarse estimtion used for light -> point contribution
float LightGrid_EstimateLightGridPerceptualContribution(MeshLightInstanceClusterHeader MLICluster, float3 GridMin, float GridSize) {
    // Estimate the contribution from the mesh light cluster using appriximated solid angle

    // Transform local cluster AABB to a conservative world-space AABB.
    float3 ClusterMin = MLICluster.AABBMin;
    float3 ClusterMax = MLICluster.AABBMax;
    float3 WorldCenter = (ClusterMin + ClusterMax) * 0.5f;
    float3 WorldExtent = max((ClusterMax - ClusterMin) * 0.5f, float3(0, 0, 0));

    // Distances from the cluster world AABB to the grid
    float3 GridCenter = GridMin + GridSize * 0.5f;
    float3 GridMax = GridMin + GridSize;

    // Min separation vector between two AABBs (cluster AABB and grid cube AABB)
    float3 ToClusterDistances = max(max(ClusterMin - GridMax, GridMin - ClusterMax), float3(0, 0, 0));
    float Distance = length(ToClusterDistances);

    float3 ToCluster = WorldCenter - GridCenter;
    float ToClusterLen = length(ToCluster);
    float WeightedNormalLenSq = dot(MLICluster.WeightedNormal, MLICluster.WeightedNormal);
    float3 WorldWeightedNormal = WeightedNormalLenSq > 1e-9f
        ? (MLICluster.WeightedNormal / sqrt(WeightedNormalLenSq))
        : 0.f.xxx;
    float3 ToClusterDir = ToClusterLen > 1e-6f ? (ToCluster / ToClusterLen) : -WorldWeightedNormal;
    float LightFacingCosineFactor = WeightedNormalLenSq > 1e-9f
        ? saturate(dot(WorldWeightedNormal, -ToClusterDir))
        : 0.f;

    float LerpingFactor = LightGrid_EstimateLightGridPerceptualContribution_ClusterNormalVarianceToLerpFactor(MLICluster.WeightedNormalVariance);
    float ClusterNearFieldRange = max(max(WorldExtent.x, max(WorldExtent.y, WorldExtent.z)) + GridSize, 1e-4f);
    float CloseRangeWeight = saturate(1.f - Distance / ClusterNearFieldRange);
    LerpingFactor = 1.f - (1.f - LerpingFactor) * (1.f - CloseRangeWeight);
    // Lerp the cosine factor to uniform distribution when MLICluster.WeightedNormalVariance is high
    {
        float Dst = 1 / LCH_PI; // PI is the integral of saturate(cosine) over the sphere. (distribute the integral uniformly on the sphere)
        LightFacingCosineFactor = lerp(LightFacingCosineFactor, Dst, LerpingFactor);
    }

    // Conservative projected-area upper bound of an AABB for any direction.
    float3 Extent = max(ClusterMax - ClusterMin, float3(0, 0, 0));
    float Ax = Extent.y * Extent.z;
    float Ay = Extent.x * Extent.z;
    float Az = Extent.x * Extent.y;
    float MaxProjectedArea = sqrt(Ax * Ax + Ay * Ay + Az * Az);

    float DistanceSq = Distance * Distance;
    float SolidAngle = LightFacingCosineFactor * MaxProjectedArea / max(DistanceSq + MaxProjectedArea / LCH_PI, 1e-6f);
    SolidAngle = min(SolidAngle, LCH_PI * LightFacingCosineFactor);

    return MLICluster.TotalIntensity / max(MLICluster.TotalArea, 1e-9f) * SolidAngle;
}

// A coarse estimtion used for light -> point contribution (incoming irradiance)
float EstimateLightContribution(MeshLightInstanceTriangle L, float3 Position, float3 Normal, bool bVolume = false) {
    
    float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3.0f;
    float3 ToLightCenter = LightCenter - Position;
    float DistanceSq = dot(ToLightCenter, ToLightCenter); 

    float3 ToLightDirection = normalize(ToLightCenter);

    float3 ToV0 = L.V0 - Position;
    float3 ToV1 = L.V1 - Position;
    float3 ToV2 = L.V2 - Position;

    // The the light is not facing the sampled position. Cull it out.
    float3 LightNormal = normalize(cross(L.V1 - L.V0, L.V2 - L.V0));
    if (dot(ToLightCenter, LightNormal) >= 0.0f) {
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
    float ReceiverCosineFactor;
    if(bVolume) {
        // Take into account the cosine factor how much the sampled point is facing towards the light
        // Regarding the nature of volume scattering, lights that the sample is not facing towards will still have a lower
        // effect on the sample. So a constant bias of 1.5 and a scaling factor of 0.4 are applied.
        ReceiverCosineFactor = saturate(CosineBias + (1.5f + dot(Normal, ToLightDirection)) * 0.4f) / PI;
        // TODO take account of different parameterizations of HG phase function
    } else {
        float k0 = saturate(dot(Normal, normalize(ToV0)));
        float k1 = saturate(dot(Normal, normalize(ToV1)));
        float k2 = saturate(dot(Normal, normalize(ToV2)));
        float MaxK = max(k0, max(k1, k2));
        // the sampled surface is not facing the light. Cull it out.
        if (MaxK <= 0.0f) {
            return 0.0f;
        }
        // Take into account the cosine factor how much the sampled point is facing towards the light
        ReceiverCosineFactor = saturate(CosineBias + MaxK);
    }

    // Take account for how well is the light facing the shading point
    float LightFacingCosineFactor = saturate(-dot(ToLightDirection, LightNormal));
    // In case the light is close to the shading point, reduce the effect from LightFacingCosineFactor
    if(LightFacingCosineFactor > 0 && DistanceSq < 1.5f * MaxLightRadiusSq)
        LightFacingCosineFactor = lerp(1, LightFacingCosineFactor, DistanceSq / max(1.5f * MaxLightRadiusSq, 1e-4f));
    
    float SolidAngleNoArea = LightFacingCosineFactor / (DistanceSq + LightArea);

    // Equivalent to L.AvgIntensity * SolidAngle * ReceiverCosineFactor, where L.AvgIntensity is L.Intensity / L.Area
    return L.Intensity * SolidAngleNoArea * ReceiverCosineFactor;
}

// A coarse estimtion used for light -> point contribution (incoming irradiance)
float EstimateLightContribution(MeshLightInstanceClusterHeader L, float3 Position, float3 Normal, bool bVolume = false) {

    // Estimate the contribution from the mesh light cluster using appriximated solid angle

    // Use the world-space AABB of the cluster as a conservative spatial proxy.
    float3 ClusterMin = L.AABBMin;
    float3 ClusterMax = L.AABBMax;
    float3 WorldCenter = (ClusterMin + ClusterMax) * 0.5f;
    float3 WorldExtent = max((ClusterMax - ClusterMin) * 0.5f, float3(0, 0, 0));

    // Closest point on the cluster AABB to the shaded position.
    float3 ClosestPoint = clamp(Position, ClusterMin, ClusterMax);
    float3 ToClosestPoint = ClosestPoint - Position;
    float ClosestDistanceSq = dot(ToClosestPoint, ToClosestPoint);
    float3 ToCluster = WorldCenter - Position;
    float ToClusterLen = length(ToCluster);
    float WeightedNormalLenSq = dot(L.WeightedNormal, L.WeightedNormal);
    float3 WorldWeightedNormal = WeightedNormalLenSq > 1e-9f
        ? (L.WeightedNormal / sqrt(WeightedNormalLenSq))
        : 0.f.xxx;
    float3 ToClusterDir = ToClusterLen > 1e-6f ? (ToCluster / ToClusterLen) : WorldWeightedNormal;
    float LightFacingCosineFactor = WeightedNormalLenSq > 1e-9f
        ? saturate(dot(WorldWeightedNormal, -ToClusterDir))
        : 0.f;

    float LerpingFactor = LightGrid_EstimateLightGridPerceptualContribution_ClusterNormalVarianceToLerpFactor(L.WeightedNormalVariance);

    // Conservative projected-area upper bound of an AABB for any direction.
    float3 Extent = max(ClusterMax - ClusterMin, float3(0, 0, 0));
    float Ax = Extent.y * Extent.z;
    float Ay = Extent.x * Extent.z;
    float Az = Extent.x * Extent.y;
    float MaxProjectedArea = sqrt(Ax * Ax + Ay * Ay + Az * Az);

    float DistanceSq = ClosestDistanceSq;


    float ReceiverCosineFactor;
    if(bVolume) {
        // For volume scattering we use the AABB as an extended emitter proxy:
        // 1) use the closest-point distance to preserve near-field contributions inside / near the box;
        // 2) relax directional attenuation when the sample is close to the cluster extent.
        float3 OutsideDistances = abs(ToClosestPoint);
        float DistanceToExitAlongAxes = max(
            max(WorldExtent.x - OutsideDistances.x, WorldExtent.y - OutsideDistances.y),
            WorldExtent.z - OutsideDistances.z
        );
        float NearFieldDistance = ClosestDistanceSq > 0.f ? sqrt(ClosestDistanceSq) : 0.f;
        if (ClosestDistanceSq <= 0.f) {
            NearFieldDistance = -DistanceToExitAlongAxes;
        }
        float ClusterNearFieldRange = max(max(WorldExtent.x, max(WorldExtent.y, WorldExtent.z)), 1e-4f);
        float NearFieldBlend = saturate(NearFieldDistance / (2.f * ClusterNearFieldRange) + 0.5f);
        float NearFieldBias = 1.f - NearFieldBlend;
        float ViewAlignment = dot(Normal, ToClusterDir);
        ReceiverCosineFactor = saturate(NearFieldBias + (1.5f + ViewAlignment) * 0.4f) / LCH_PI;

        // Close to the cluster, the AABB proxy becomes less directional than the weighted normal alone suggests.
        float DirectionalityBlend = saturate(NearFieldDistance / (1.5f * ClusterNearFieldRange) + 0.5f);
        LightFacingCosineFactor = lerp(1.f, LightFacingCosineFactor, DirectionalityBlend);
        LightFacingCosineFactor = lerp(
            LightFacingCosineFactor,
            1.f / LCH_PI,
            0.5f * LerpingFactor
        );
    } else {
        // Use the closest point on the cluster AABB for the receiver cosine check.
        // This is more robust than the far-corner (SelectedVertex) approach, which can place
        // the reference point behind the surface when the shading point is near or just outside
        // the AABB boundary (common for large clusters like light spheres close to geometry).
        float3 ToClosest = ClosestPoint - Position;
        float ClosestDist = length(ToClosest);
        float MaxK;
        if (ClosestDist > 1e-6f) {
            MaxK = saturate(dot(Normal, ToClosest / ClosestDist));
        } else {
            // Position is inside or on the AABB surface — every direction sees part of the cluster.
            MaxK = 1.0f;
        }

        // Near-field relaxation: when the surface is close to the cluster extent, the AABB proxy
        // becomes less directional. Relax the receiver cosine toward uniform, analogous to the
        // CosineBias in the triangle version and the CloseRangeWeight used for LightFacingCosineFactor.
        float ClusterNearFieldRange = max(max(WorldExtent.x, max(WorldExtent.y, WorldExtent.z)), 1e-4f);
        float CloseRangeWeight = saturate(1.f - ClosestDist / ClusterNearFieldRange);
        MaxK = lerp(MaxK, 1.0f, CloseRangeWeight);

        // the sampled surface is not facing the light. Cull it out.
        if (MaxK <= 0.0f) {
            return 0.0f;
        }
        ReceiverCosineFactor = MaxK;

        // Close to the cluster, the weighted-normal proxy is too directional for large curved emitters.
        // Relax it towards an isotropic emitter similarly to grid injection.
        LerpingFactor = 1.f - (1.f - LerpingFactor) * (1.f - CloseRangeWeight);
    }

    // Lerp the cosine factor to uniform distribution when the cluster is sufficiently isotropic
    // or when the shading point is close to its spatial extent.
    {
        float Dst = 1 / LCH_PI; // PI is the integral of saturate(cosine) over the sphere. (distribute the integral uniformly on the sphere)
        LightFacingCosineFactor = lerp(LightFacingCosineFactor, Dst, LerpingFactor);
    }

    float SolidAngle = LightFacingCosineFactor * MaxProjectedArea / max(DistanceSq + MaxProjectedArea / LCH_PI, 1e-6f);
    
    SolidAngle = min(SolidAngle, LCH_PI * LightFacingCosineFactor);

    return L.TotalIntensity * ReceiverCosineFactor / max(L.TotalArea, 1e-9f) * SolidAngle;
}

EvaluatedAreaLight LCH_ExtractTriangleLight (MeshLight ML, MeshLightInstance MLI, MeshLightTriangle MLTriangle) {
    AreaLight InAreaLight;
    InAreaLight.RenderableIndex = MLI.RenderableIndex;
    InAreaLight.StaticMeshDescriptionIndex = ML.StaticMeshDescriptionIndex;
    InAreaLight.PrimitiveIndex = MLTriangle.PrimitiveIndex;
    InAreaLight.Flags = 0; // Useless when unpacking
    bool bActive;
    return EvaluateLight(InAreaLight, bActive);
}

EvaluatedAreaLight LCH_SampleAndEvaluateLight (
    // uint MLClusterOffset, uint MLIClusterNodeOffset,
    MeshLightInstanceElementOffset AbsElement, float u, out float Pdf
) {
    uint MLClusterOffset, MLIClusterOffset;
    MeshLight ML;
    MeshLightInstance MLI;
    if(AbsElement.bIsTriangle()) {
        MeshLightInstanceTriangle Triangle = LCH_MeshLightInstanceTriangleBuffer[AbsElement.Offset()];
        MLI = LCH_MeshLightInstanceBuffer[Triangle.MeshLightInstanceIndex];
        ML  = LCH_MeshLightBuffer[MLI.MeshLightIndex];
    } else {
        MeshLightInstanceClusterHeader ClusterHeader = LCH_MeshLightInstanceClusterHeaderBuffer[AbsElement.Offset()];
        MLI = LCH_MeshLightInstanceBuffer[ClusterHeader.MeshLightInstanceIndex];
        ML  = LCH_MeshLightBuffer[MLI.MeshLightIndex];
    }
    uint MLIClusterNodeOffset = MLI.MeshLightInstanceClusterOffset.Offset();
    Pdf = 1.f;
    MeshLightInstanceElementOffset RelElement = MakeMeshLightInstanceElementOffset(
        AbsElement.bIsTriangle(), AbsElement.Offset() 
        - (AbsElement.bIsTriangle() ? MLI.MeshLightInstanceTriangleOffset : MLIClusterNodeOffset)
    );
    while(!RelElement.bIsTriangle()) {
        MeshLightInstanceClusterNode ClusterNode = LCH_MeshLightInstanceClusterNodeBuffer[MLIClusterNodeOffset + RelElement.Offset()];
        MeshLightClusterNode MLClusterNode = LCH_MeshLightClusterNodeBuffer[ML.ClusterOffset + RelElement.Offset()];
        if(u < ClusterNode.L_Probability) {
            // Go to the left child
            RelElement = MakeMeshLightInstanceElementOffset(MLClusterNode.L.bIsLeaf(), MLClusterNode.L.Index());
            u = u / ClusterNode.L_Probability;
            Pdf = Pdf * ClusterNode.L_Probability;
        } else {
            // Go to the right child
            RelElement = MakeMeshLightInstanceElementOffset(MLClusterNode.R.bIsLeaf(), MLClusterNode.R.Index());
            u = (u - ClusterNode.L_Probability) / (1.f - ClusterNode.L_Probability);
            Pdf = Pdf * (1.f - ClusterNode.L_Probability);
        }
    }
    // Extract and evaluate triangle
    MeshLightTriangle MLTriangle = LCH_MeshLightTriangleBuffer[
        RelElement.Offset() + ML.TriangleOffset
    ];
    return LCH_ExtractTriangleLight(ML, MLI, MLTriangle);
}

#endif // LIGHT_CLUSTER_HIERARCHY_RESOURCES_HLSL
