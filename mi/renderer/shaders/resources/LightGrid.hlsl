#ifndef LIGHT_GRID_HLSL
#define LIGHT_GRID_HLSL

#include "../headers/Conventions.hlsl"
#include "../headers/Math.hlsl"
#include "../headers/Light.hlsl"

// This must be consistent with the value within r_diffuse_direct_lighting.cpp
#define LIGHT_GRID_NUM_CASCADES 6

#define MAX_NUM_LIGHT_CASCADES 8
#if LIGHT_GRID_NUM_CASCADES > MAX_NUM_LIGHT_CASCADES
#error "LIGHT_GRID_NUM_CASCADES must be less than or equal to MAX_NUM_LIGHT_CASCADES"
#endif

struct LightStructureUB {
    uint3 LightGridSize;
    float LightGridCellSize;
    float3 LightGridCenter;
    uint LighGridNumCascadesUsed;
    uint LightGridMaxNumGridLights;
    uint LightGridNumCascadeGrids;
    uint LightGridNumGrids;
    float LightInjectionIntensityThreshold;
    float4 LightGridCascadeMin[LIGHT_GRID_NUM_CASCADES];
    float4 LightGridCascadeMax[LIGHT_GRID_NUM_CASCADES];
    uint FrameIndex;
    uint MaxNumLights;
    uint LightGridMaxNumEntries;
    float EnvironmentLightHemisphereSampleLOD;
    float LightCullingRate;
    float3 EnvironmentLightMultiplier;
    float  EnvironmentLightEvaluateLOD;
    uint3  Padding;
};

ConstantBuffer<LightStructureUB> LightStructure_UB;

struct DirectLightingUB {
    uint FrameIndex;
    float ShadowRayTMax;
    float ShadowRayLengthMultiplier;
    uint Unused;
};

// returns the wold grid min
float3 LightGrid_GetGridBounds(int4 GridIndex, out float GridSize) {
    GridSize = LightStructure_UB.LightGridCellSize * pow(2, GridIndex.w);
    return LightStructure_UB.LightGridCascadeMin[GridIndex.w].xyz
         + GridSize * GridIndex.xyz;
}

bool LightGrid_IsInsideGrid(int4 GridIndex, float3 Position) {
    float GridSize;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    return all(Position >= GridMin) && all(Position < GridMin + GridSize);
}

bool LightGrid_IsInsideAnyCascade(float3 Position) {
    // Check if the position is inside the largest cascade of grids
    return all(Position >= LightStructure_UB.LightGridCascadeMin[LIGHT_GRID_NUM_CASCADES - 1].xyz)
        && all(Position < LightStructure_UB.LightGridCascadeMax[LIGHT_GRID_NUM_CASCADES - 1].xyz);
}

uint4 LightGrid_GetGridIndex(float3 Position) {
    float GridSize = LightStructure_UB.LightGridCellSize;

    [unroll(LIGHT_GRID_NUM_CASCADES)]
    for (uint i = 0; i < LightStructure_UB.LighGridNumCascadesUsed; i++) {
        if (all(Position >= LightStructure_UB.LightGridCascadeMin[i].xyz)
         && all(Position < LightStructure_UB.LightGridCascadeMax[i].xyz)) {
            return uint4(
                uint3((Position - LightStructure_UB.LightGridCascadeMin[i].xyz) / GridSize),
                i
            );
        }
        GridSize = GridSize * 2;
    }
    return INVALID_UINT.xxxx;
}

uint LightGrid_GetGridIndex1 (uint4 GridIndex) {
    return GridIndex.x + GridIndex.y * LightStructure_UB.LightGridSize.x
    + GridIndex.z * LightStructure_UB.LightGridSize.x * LightStructure_UB.LightGridSize.y
    + GridIndex.w * LightStructure_UB.LightGridNumCascadeGrids;
}

uint4 LightGrid_GetGridIndex(uint GridIndex1) {
    uint4 GridIndex = uint4(
        GridIndex1 % LightStructure_UB.LightGridSize.x,
        GridIndex1 / LightStructure_UB.LightGridSize.x % LightStructure_UB.LightGridSize.y,
        GridIndex1 / (LightStructure_UB.LightGridSize.x * LightStructure_UB.LightGridSize.y) % LightStructure_UB.LightGridSize.z,
        GridIndex1 / LightStructure_UB.LightGridNumCascadeGrids
    );
    return GridIndex;
}


// A coarse estimtion used for light grid injection
// Estimate Light -> grid contribution
float LightGrid_EstimateLightGridPerceptualContribution(PrecomputedLight L, float3 GridMin, float GridSize) {
    // Estimate the contribution from the area light using appriximated solid angle
    // Here, L.PerceptualIntensity is the luminance of the light x the area of the light

    // Calculate the distance from the light to the grid
    float3 GridCenter = GridMin + GridSize * 0.5f;
    float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3;
    float3 LightToGrid = GridCenter - LightCenter;
    float Distance = length(LightToGrid);
    float3 LightToGridDirection;
    {
        // Offset the grid center when computing Light-Grid direction for conservative estimation
        float3 LightToGridDirectionUnnormalized = LightToGrid + (L.Normal * sqrt(3.f) * 0.6f * GridSize);
        float  Len = length(LightToGridDirectionUnnormalized);
        if(Len > 1e-6f) {
            LightToGridDirection = LightToGridDirectionUnnormalized / Len;
        } else {
            // The light is very close to the grid center. Fallback to light normal
            LightToGridDirection = L.Normal;
        }
    }
    float LightFacingCosineFactor = saturate(dot(L.Normal, LightToGridDirection));

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
    float SolidAngle = LightArea * LightFacingCosineFactor / max(EffectiveDistanceSq + LightArea / PI, 1e-6f);

    // Area has been taken account by L.PerceptualIntensity
    return L.PerceptualIntensity * SolidAngle;
}

#endif