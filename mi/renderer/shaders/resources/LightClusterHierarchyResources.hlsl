#include "../shared/SharedLightClusterHierarchy.hlsl"

StructuredBuffer<MeshLightTriangle> LCH_MeshLightTriangleBuffer;
StructuredBuffer<MeshLightTriangleBakedData> LCH_MeshLightTriangleBakedDataBuffer;
StructuredBuffer<MeshLightClusterHeader> LCH_MeshLightClusterHeaderBuffer;
StructuredBuffer<MeshLightClusterNode> LCH_MeshLightClusterNodeBuffer;

StructuredBuffer<MeshLightInstance> LCH_MeshLightInstanceBuffer;
StructuredBuffer<MeshLight> LCH_MeshLightBuffer;
StructuredBuffer<MeshLightLevelHeader> LCH_MeshLightLevelHeaderBuffer;


// Nodes for each instance cluster.
StructuredBuffer<uint> LCH_InstanceClusterNodeOffsetBuffer;
StructuredBuffer<MeshLightInstanceClusterHeader> LCH_MeshLightInstanceClusterHeaderBuffer;
StructuredBuffer<MeshLightInstanceClusterNode> LCH_MeshLightInstanceClusterNodeBuffer;


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

    float3 ClusterMin = WorldCenter - WorldExtent;
    float3 ClusterMax = WorldCenter + WorldExtent;

    // Distances from the cluster world AABB to the grid
    float3 GridCenter = GridMin + GridSize * 0.5f;
    float3 GridMax = GridMin + GridSize;

    // Min separation vector between two AABBs (cluster AABB and grid cube AABB)
    float3 ToClusterDistances = max(max(ClusterMin - GridMax, GridMin - ClusterMax), float3(0, 0, 0));
    float Distance = length(ToClusterDistances);

    float3 ToCluster = WorldCenter - GridCenter;
    float ToClusterLen = length(ToCluster);
    float3 WorldWeightedNormal = normalize(mul(LinearPart, MLICluster.WeightedNormal));
    float3 ToClusterDir = ToClusterLen > 1e-6f ? (ToCluster / ToClusterLen) : -WorldWeightedNormal;
    float LightFacingCosineFactor = saturate(dot(WorldWeightedNormal, -ToClusterDir));

    float LerpingFactor = LightGrid_EstimateLightGridPerceptualContribution_ClusterNormalVarianceToLerpFactor(MLICluster.WeightedNormalVariance);
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
