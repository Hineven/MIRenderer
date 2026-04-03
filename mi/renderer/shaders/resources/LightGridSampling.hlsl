#ifndef LIGHT_GRID_SAMPLING_HLSL
#define LIGHT_GRID_SAMPLING_HLSL
#include "../shared/SharedLightClusterHierarchy.hlsl"
#include "../headers/Random.hlsl"
#include "../headers/Scattering.hlsl"
#include "../headers/OctahedronMapping.hlsl"
#include "../headers/Sampling.hlsl"
#include "../headers/Scattering.hlsl"
#include "../headers/Conventions.hlsl"
#include "LightEvaluation.hlsl"
#include "LightClusterHierarchyResources.hlsl"

#include "LightGrid.hlsl"
#include "EnvironmentLightResource.hlsl"
#include "DirectionalLightResource.hlsl"

// Input macros
#ifndef MAX_NUM_GRID_LIGHTS
#define MAX_NUM_GRID_LIGHTS 32
#endif

#ifndef NUM_LIGHT_SAMPLER_SAMPLES
#define NUM_LIGHT_SAMPLER_SAMPLES 6
#endif

#ifndef LIGHT_GRID_NUM_HISTORY_FRAMES
#define LIGHT_GRID_NUM_HISTORY_FRAMES 4
#endif

// Flag grids that are active in the current frame. 
RWStructuredBuffer<uint> LightGrid_RWActiveGridFlagBuffer;
// Record the status for the "pressure" of each light grid. This is related to how subdivided the MLI
// clusters are for the grid. Grids with higher lighting pressure will have coarser clusters.
RWStructuredBuffer<uint2> LightGrid_RWGridPressureBuffer;

struct LightGridPressureContext {
    float IntensityThreshold;
    uint  PreviousNumLights;
    uint  CurrentNumLights;
    bool  bCanFurtherSubdivide;
};
LightGridPressureContext LightGridPressureContext_Init(uint2 GridPressure) {
    LightGridPressureContext Context = (LightGridPressureContext)0;
    Context.IntensityThreshold = asfloat(GridPressure.x);
    Context.PreviousNumLights = GridPressure.y;
    return Context;
}
uint2 LightGrid_UpdateGridPressure(LightGridPressureContext Context) {
    uint2 NewGridPressure = 0;
    NewGridPressure.x = asuint(Context.IntensityThreshold);
    NewGridPressure.y = Context.CurrentNumLights;
    return NewGridPressure;
}

// A list of currently active grids
RWStructuredBuffer<uint> LightGrid_RWActiveGridAllocator;
RWStructuredBuffer<uint> LightGrid_RWActiveGridIndicesBuffer;

// Number of active mesh light instances. This is uploaded from CPU.
RWStructuredBuffer<uint> LightGrid_RWActiveMeshLightInstanceCount;
StructuredBuffer<uint> LightGrid_ActiveMeshLightInstanceIndexBuffer;

// Allocator for the light list elements of each grid. 
RWStructuredBuffer<uint>  LightGrid_RWListAllocator;
// Can be either MLI triangle or MLI cluster reference. They should be absolute offsets in the buffer.
RWStructuredBuffer<MeshLightInstanceElementOffset>  LightGrid_RWListMeshLightInstanceElementIndexBuffer;
RWStructuredBuffer<uint> LightGrid_RWGridLightListOffsetBuffer;
RWStructuredBuffer<float> LightGrid_RWGridLightListCdfBuffer;
RWStructuredBuffer<uint> LightGrid_RWGridLightListLengthBuffer;
// Record the importance of environment light grids (2x2x6 cubic tiles)
// (low end) | +x | -x | +y | -y | +z | -z | (high end) 
// for each individual face with axis t, p = t+1, q = t+2, the 4 bits are arranged as:
// 0: (p+, q+), 1: (p+, q-), 2: (p-, q+), 3: (p-, q-) 
RWStructuredBuffer<uint> LightGrid_RWEnvironmentVisibilityHistoryBuffer;
// Record the combination of light encodings that successfully illuminated geometries in the grid
// 64 bits per grid cell per frame
RWStructuredBuffer<uint2> LightGrid_RWBloomFilterBuffer;

RWStructuredBuffer<uint2> LightGrid_RWNextBloomFilterBuffer;
RWStructuredBuffer<uint>  LightGrid_RWNextEnvironmentVisibilityBuffer;

static const uint LIGHT_SAMPLE_INDEX_DIRECTIONAL = INVALID_UINT - 1u;

bool IsDirectionalLightSampleIndex(uint LightIndex) {
    return LightIndex == LIGHT_SAMPLE_INDEX_DIRECTIONAL;
}

struct LightSampler {
    uint NumResampledLights;
    float SumWeight;
    float SampleU[NUM_LIGHT_SAMPLER_SAMPLES];
    float Weights[NUM_LIGHT_SAMPLER_SAMPLES];
    uint  PackedSamplerLights[NUM_LIGHT_SAMPLER_SAMPLES];
};

struct LightSamplerLight {
    // The element (MLI triangle / cluster) to sample from
    MeshLightInstanceElementOffset AbsElementIndex;
    bool bIsEnvironment;
    bool bIsDirectional;
    bool bValid;
};

uint PackLightSamplerLight(LightSamplerLight LSL) {
    if (!LSL.bValid) {
        return INVALID_UINT;
    }
    uint Packed = 0;
    // We only have 29 bits for the element index. Anyway it will not be a problem since there're 
    // usually much less than 512M triangles/clusters in the scene.
    Packed |= LSL.AbsElementIndex.Packed & 0x1FFFFFFFu;
    Packed |= LSL.AbsElementIndex.bIsTriangle() ? (0x20000000u) : 0u;
    Packed |= (LSL.bIsEnvironment ? 1u : 0u) << 30;
    Packed |= (LSL.bIsDirectional ? 1u : 0u) << 31;
    return Packed;
}

LightSamplerLight UnpackLightSamplerLight(uint Packed) {
    LightSamplerLight LSL = (LightSamplerLight)0;
    LSL.bValid = (Packed != INVALID_UINT);
    if (LSL.bValid) {
        LSL.AbsElementIndex = MakeMeshLightInstanceElementOffset((Packed & 0x20000000u) != 0, Packed & 0x1FFFFFFFu);
        LSL.bIsEnvironment = ((Packed >> 30) & 0x1) != 0;
        LSL.bIsDirectional = ((Packed >> 31) & 0x1) != 0;
    }
    return LSL;
}

LightSamplerLight MakeInvalidLightSamplerLight() {
    LightSamplerLight LSL = (LightSamplerLight)0;
    LSL.bValid = false;
    return LSL;
}

LightSampler InitLightSampler(inout Random R) {
    LightSampler LS = (LightSampler)0;
    // Scatter samples
    for (int i = 0; i < NUM_LIGHT_SAMPLER_SAMPLES; i++) {
        float Step = (1.f / NUM_LIGHT_SAMPLER_SAMPLES);
        LS.SampleU[i] = saturateDown((R.rand() + i) * Step);
        LS.PackedSamplerLights[i] = PackLightSamplerLight(MakeInvalidLightSamplerLight());
        LS.Weights[i] = 0.f;
    }
    return LS;
}

void LightSampler_AddLightToSampler(inout LightSampler LS, float Weight, LightSamplerLight LSL) {
    float U = Weight / (LS.SumWeight + Weight + 1e-6f);
    LS.NumResampledLights ++;
    LS.SumWeight += Weight;
    for (uint i = 0; i < NUM_LIGHT_SAMPLER_SAMPLES; i++) {
        bool bSelect = false;
        if (U > LS.SampleU[i]) bSelect = true;
        if (bSelect) {
            LS.PackedSamplerLights[i] = PackLightSamplerLight(LSL);
            LS.SampleU[i] = LS.SampleU[i] / U;
            LS.Weights[i] = Weight;
        } else {
            LS.SampleU[i] = saturateDown((LS.SampleU[i] - U) / (1.f - U));
        }
    }
}

void LightSampler_AddListLightToSampler(
    inout LightSampler LS, float Weight,
    MeshLightInstanceElementOffset AbsElementIndex 
) {
    LightSamplerLight LSL = (LightSamplerLight)0;
    LSL.AbsElementIndex   = AbsElementIndex;
    LSL.bIsEnvironment = false;
    LSL.bIsDirectional = false;
    LSL.bValid = true;
    LightSampler_AddLightToSampler(LS, Weight, LSL);
}

void LightSampler_AddDirectionalLightToSampler(inout LightSampler LS, float Weight) {
    LightSamplerLight LSL = (LightSamplerLight)0;
    LSL.bIsEnvironment = false;
    LSL.bIsDirectional = true;
    LSL.bValid = true;
    LightSampler_AddLightToSampler(LS, Weight, LSL);
}

struct LightSampleSrcLightRecord {
    // Highest 2 bits: 0: MLI triangle, 1: MLI cluster, 2: environment, 3: directional 

#define LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_MLI_TRIANGLE 0
#define LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_MLI_CLUSTER 1
#define LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_ENVIRONMENT 2
#define LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_DIRECTIONAL 3

    // INVALID_UINT means invalid light
    uint Packed;
    bool IsMeshLightTriangle() {
        return (Packed & 0xc0000000u) == LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_MLI_TRIANGLE;
    }
    bool IsMeshLightCluster() {
        return (Packed & 0xc0000000u) == LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_MLI_CLUSTER;
    }
    bool IsEnvironmentLight() {
        return (Packed & 0xc0000000u) == LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_ENVIRONMENT;
    }
    bool IsDirectionalLight() {
        return (Packed & 0xc0000000u) == LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_DIRECTIONAL;
    }
    uint Index () {
        return Packed & 0x3FFFFFFFu;
    }
    bool IsValid() {
        return Packed != INVALID_UINT;
    }
};

LightSampleSrcLightRecord MakeInvalidLightSampleRecord() {
    LightSampleSrcLightRecord Record = (LightSampleSrcLightRecord)0;
    Record.Packed = INVALID_UINT;
    return Record;
}

LightSampleSrcLightRecord MakeLightSampleRecord (uint Type, uint Index) {
    LightSampleSrcLightRecord Record = (LightSampleSrcLightRecord)0;
    Record.Packed = (Type << 30) | (Index & 0x3FFFFFFFu);
    return Record;
}

LightSampleSrcLightRecord MakeEnvironmentLightSampleRecord() {
    return MakeLightSampleRecord(LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_ENVIRONMENT, 0);
}

LightSampleSrcLightRecord MakeDirectionalLightSampleRecord() {
    return MakeLightSampleRecord(LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_DIRECTIONAL, 0);
}

LightSampleSrcLightRecord MakeAreaLightSampleRecord(MeshLightInstanceElementOffset AbsElement) {
    return MakeLightSampleRecord(
        AbsElement.bIsTriangle() 
        ? LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_MLI_TRIANGLE 
        : LIGHT_SAMPLE_SRC_LIGHT_RECORD_TYPE_MLI_CLUSTER, 
        AbsElement.Offset()
    );
}



struct LightSample {
    // For area light: sampled position on the light
    // For environment light: sampled normalized direction
    float3 Position;
    // Pdf in solid angle domain. For area light, it is multiplied by LightGridLightListCdf to account for overflowing lights that are not injected into the grid
    float Pdf;
    float3 Radiance;
    // Keep the hash record of the sampled light.
    LightSampleSrcLightRecord LightRecord;
    bool bIsEnvironmentLightSample;
    bool bIsDirectionalLightSample;
    // Returns true if the sample is valid: pdf > 0
    bool IsValid() {
        return Pdf > 0;
    }
    bool IsEnvironmentLight() {
        return bIsEnvironmentLightSample;
    }
    bool IsInfiniteLight() {
        return bIsEnvironmentLightSample || bIsDirectionalLightSample;
    }
};

float3 SampleAreaLightArea(float3 V0, float3 V1, float3 V2, float2 u, out float Area, out float AreaPdf) {
    if (u.x + u.y > 1.f) {
        u = 1.f - u;
    }
    float3 Position = InterpolateBarycentrics(V0, V1, V2, u);
    Area = length(cross(V1 - V0, V2 - V0)) / 2.f;
    AreaPdf = 1.f / max(Area, 1e-5f);
    return Position;
}

// sample a light with history info from an area light with pre-multiplied cosine / phase function
LightSample SampleAreaLightDiffuseWithPreMultiplied(
    float3 Position, float3 Normal, float3 ViewDirection, EvaluatedAreaLight Evaluated, 
    bool bSurface,
    float g, float2 u2
) {
    LightSample Result = (LightSample)0;
    float Area = 0.f;
    Result.Position = SampleAreaLightArea(Evaluated.V0, Evaluated.V1, Evaluated.V2, u2, Area, Result.Pdf);
    float2 UV = InterpolateBarycentrics(Evaluated.UV0, Evaluated.UV1, Evaluated.UV2, u2);
    float Distance = length(Result.Position - Position);
    float3 Direction = normalize(Result.Position - Position);
    float3 LightNormal = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    float LightCosine = dot(LightNormal, -Direction);
    float ReceiverCosine = bSurface ? dot(Direction, Normal) : dot(Direction, ViewDirection);
    // Convert surface domain pdf to solid angle domain pdf
    Result.Pdf *= Distance * Distance / max(abs(LightCosine), 1e-4f);
    float3 EvaluatedEmission = Evaluated.Emission;
    if (IsValid(Evaluated.EmissionTextureIndex))
        EvaluatedEmission += GetBindlessSRV(Evaluated.EmissionTextureIndex).SampleLevel(LinearWrapSampler, UV, 0).rgb;
    float PreMultipliedCosine = saturate(ReceiverCosine);
    float PreMultipliedHG = HenyeyGreensteinPhaseFunction(ReceiverCosine, g);
    float PreMultiplied = bSurface ? PreMultipliedCosine : PreMultipliedHG;
    Result.Radiance = EvaluatedEmission * PreMultiplied;
    return Result;
}

// Importance ample a light with history info from the environment light with pre-multiplied cosine / phase function
LightSample SampleEnvironmentLightDiffuseWithPreMultiplied(
    float3 Normal, float3 ViewDirection, 
    bool bSurface,
    float g, float2 u2
) {
    LightSample Result = (LightSample)0;
    Result.bIsEnvironmentLightSample = true;
    float3 LocalDirection;
    if(bSurface) {
        LocalDirection = SampleHemisphereCosineWeighted(u2, Result.Pdf);
    } else {
        LocalDirection = SampleHenyeyGreenstein(g, u2, Result.Pdf);
    }
    float3 Tangent, Bitangent;
    GetOrthoVectors(Normal, Tangent, Bitangent);
    float3 Direction = Tangent * LocalDirection.x + Bitangent * LocalDirection.y + Normal * LocalDirection.z; 
    Result.Position = Direction;
    float ReceiverCosine = bSurface ? LocalDirection.z : dot(Direction, ViewDirection);
    float3 EvaluatedEmission = EvaluateEnvironmentMap(-Direction).rgb;
    float PreMultiplied = bSurface ? saturate(ReceiverCosine) : HenyeyGreensteinPhaseFunction(ReceiverCosine, g);
    Result.Radiance = EvaluatedEmission * PreMultiplied;
    return Result;
}

LightSample SampleAreaLightDiffuseWithPreMultipliedCosine(float3 Position, float3 Normal, EvaluatedAreaLight Evaluated, float2 u2) {
    return SampleAreaLightDiffuseWithPreMultiplied(
        Position, Normal, float3(0,0,0), Evaluated, true, 0.f, u2
    );
}

LightSample SampleLightWithPreMultipliedPhaseFunction(
    float3 Position, float3 ViewDirection, EvaluatedAreaLight Evaluated, float g, float2 u2
) {
    return SampleAreaLightDiffuseWithPreMultiplied(
        Position, float3(0,0,0), ViewDirection, Evaluated, false, g, u2
    );
}

LightSample SampleDirectionalLightDiffuseWithPreMultiplied(
    float3 Normal, float3 ViewDirection,
    bool bSurface,
    float g
) {
    LightSample Result = (LightSample)0;
    if (!DirectionalLightEnabled()) {
        return Result;
    }
    Result.bIsDirectionalLightSample = true;
    Result.Position = GetDirectionalLightDirection();
    Result.Pdf = 1.f;

    float ReceiverCosine = bSurface ? dot(Result.Position, Normal) : dot(Result.Position, ViewDirection);
    float PreMultiplied = bSurface ? saturate(ReceiverCosine) : HenyeyGreensteinPhaseFunction(ReceiverCosine, g);
    Result.Radiance = GetDirectionalLightIrradiance() * PreMultiplied;
    return Result;
}

struct LightGrid_CubicVisibility {
    uint GridCubicHistory[LIGHT_GRID_NUM_HISTORY_FRAMES];
};

LightGrid_CubicVisibility LightGrid_FetchEnvironmentVisibility(uint GridIndex1) {
    LightGrid_CubicVisibility Visibility = (LightGrid_CubicVisibility)0;
    [unroll(LIGHT_GRID_NUM_HISTORY_FRAMES)]
    for (uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
        Visibility.GridCubicHistory[i] = LightGrid_RWEnvironmentVisibilityHistoryBuffer[GridIndex1 * LIGHT_GRID_NUM_HISTORY_FRAMES + i];
    }
    return Visibility;
}

struct LightGrid_GridLightVisibility {
    uint2 BloomFilters[LIGHT_GRID_NUM_HISTORY_FRAMES];
};

LightGrid_GridLightVisibility GetGridLightVisibility(uint GridIndex1) {
    LightGrid_GridLightVisibility GridVisibility = (LightGrid_GridLightVisibility)0;
    [unroll(LIGHT_GRID_NUM_HISTORY_FRAMES)]
    for (uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
        GridVisibility.BloomFilters[i] = LightGrid_RWBloomFilterBuffer[GridIndex1 * LIGHT_GRID_NUM_HISTORY_FRAMES + i];
    }
    return GridVisibility;
}

float LightGrid_GridLightVisibilityWeight(LightGrid_GridLightVisibility GridVisibility, uint2 Hash) {
    float Weight = 0.f;
    [unroll(LIGHT_GRID_NUM_HISTORY_FRAMES)]
    for(uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
        uint2 BloomFilter = GridVisibility.BloomFilters[i];
        if (all((BloomFilter & Hash) == Hash)) {
            Weight += 1;
        }
    }
    return max(Weight / LIGHT_GRID_NUM_HISTORY_FRAMES, 0.1f);
}

uint LightGrid_GetInfiniteVisibilityBitIndex(float3 Direction) {
    float3 AbsDirection = abs(Direction);
    uint FaceIndex = 0;
    float2 SubDirection;
    if (AbsDirection.x >= AbsDirection.y && AbsDirection.x >= AbsDirection.z) {
        FaceIndex = Direction.x > 0 ? 0 : 1;
        SubDirection = Direction.yz;
    } else if (AbsDirection.y >= AbsDirection.x && AbsDirection.y >= AbsDirection.z) {
        FaceIndex = Direction.y > 0 ? 2 : 3;
        SubDirection = Direction.zx;
    } else {
        FaceIndex = Direction.z > 0 ? 4 : 5;
        SubDirection = Direction.xy;
    }
    uint2 SubIndex = uint2(
        SubDirection.x > 0 ? 0u : 1u,
        SubDirection.y > 0 ? 0u : 1u
    );
    return FaceIndex * 4 + SubIndex.x * 2 + SubIndex.y;
}

float LightGrid_EstimateInfiniteLightVisibility(float3 Direction, LightGrid_CubicVisibility GridCubicVisibility) {
    return 1.f;
}

// bSurface: if the sample is sampled for surface shading (otherwise we assume volume shading and WorldNormal is omitted)
// bGroupedAccess: whether locality is assumed when accessing the light grid for each wave. You can enable this when you 
// know that the threads in a wave will access similar grid cells (e.g., tiled rendering on screen)
// bWithEnvironment: whether environment light is considered and can be sampled
LightSample SampleOneLightSample_RIS (
    float3 WorldPosition, float3 WorldNormal, float3 ViewDirection,
    bool bSurface, bool bGroupedAccess, bool bWithEnvironment, bool bWithDirectional,
    inout Random R, 
    out float3 RadianceEstimation,
    out float SumResampleWeights, out uint NumValidSamples,
    out float LightGridLightListCdf
) {
    NumValidSamples = 0;
    SumResampleWeights = 0.f;
    LightGridLightListCdf = 1.f;

    // Look up the light grid
    uint4 GridIndex = LightGrid_GetGridIndex(WorldPosition);
    LightSample Sample = (LightSample)0;
    bool bInsideLightGridBounds = IsValid(GridIndex.x);
    uint GridIndex1 = bInsideLightGridBounds ? LightGrid_GetGridIndex1(GridIndex) : 0;

    LightSampler LS = InitLightSampler(R);

    float GridSize = 0;
    float3 GridMin = 0.xxx;
    uint NumGridLights = 0;
    uint GridLightListOffset = 0;
    LightGrid_GridLightVisibility GridVisibility = (LightGrid_GridLightVisibility)0;
    if (bInsideLightGridBounds) {
        GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
        NumGridLights = LightGrid_RWGridLightListLengthBuffer[GridIndex1];
        GridLightListOffset = LightGrid_RWGridLightListOffsetBuffer[GridIndex1];
        uint MaxNumEntries = max(LightStructure_UB.LightGridMaxNumEntries, 1u);
        if (GridLightListOffset >= MaxNumEntries) {
            NumGridLights = 0;
        } else {
            NumGridLights = min(NumGridLights, MaxNumEntries - GridLightListOffset);
        }
        LightGridLightListCdf = LightGrid_RWGridLightListCdfBuffer[GridIndex1];
        GridVisibility = GetGridLightVisibility(GridIndex1);

        // Mark the grid as visited. The grid will be injected with lights in the next few frame.
        InterlockedOr(LightGrid_RWActiveGridFlagBuffer[GridIndex1], 1u);
    }

    bool bHasGridLights = bInsideLightGridBounds && NumGridLights > 0;

    // Spawn candidate samples from the lights in the grid
    uint NumNonZeroGridLights = 0;
    if (bHasGridLights) {
        bool bUniformGrid = WaveActiveAllEqual(GridIndex1);
        if (bUniformGrid || !bGroupedAccess) {
            // Assume one wave have locality regarding the grid index
            for (uint LightListIndex = 0; LightListIndex < NumGridLights; LightListIndex++) {
                MeshLightInstanceElementOffset Element = LightGrid_RWListMeshLightInstanceElementIndexBuffer[GridLightListOffset + LightListIndex];
                float Weight = 0;
                uint Hash = 0;
                if(Element.bIsTriangle()) {
                    MeshLightInstanceTriangle L = LCH_MeshLightInstanceTriangleBuffer[Element.Offset()];
                    Weight = EstimateLightContribution(L, WorldPosition, WorldNormal, !bSurface);
                    Hash = L.Hash;
                } else {
                    MeshLightInstanceClusterHeader Cluster = LCH_MeshLightInstanceClusterHeaderBuffer[Element.Offset()];
                    Weight = EstimateLightContribution(Cluster, WorldPosition, WorldNormal, !bSurface);
                    Hash = Cluster.Hash;
                }
                // Estimate history visibility weight
                float VisibilityWeight = LightGrid_GridLightVisibilityWeight(GridVisibility, Hash);
                // FIXME
                VisibilityWeight = 1;
                Weight *= VisibilityWeight;
                if(Weight > 0.f) {
                    LightSampler_AddListLightToSampler(LS, Weight, Element);
                    NumNonZeroGridLights ++;
                }
            }
        } else {
            // Process the light with the minimum index in the grid
            uint LightListIndex = 0, Iteration = 0;
            // TODO remove Iteration (used to prevent driver timeouts)
            // TODO add a noisy occlusion modifier based on history cache to the target distribution
            while(LightListIndex < NumGridLights && Iteration < 256) {
                MeshLightInstanceElementOffset Element = (MeshLightInstanceElementOffset)INVALID_UINT;
                Element = LightGrid_RWListMeshLightInstanceElementIndexBuffer[GridLightListOffset + LightListIndex];
                uint WaveMinLightIndex = WaveActiveMin(Element.Packed);
                if (WaveMinLightIndex == Element.Packed) {
                    float Weight = 0;
                    uint Hash = 0;
                    if(Element.bIsTriangle()) {
                        MeshLightInstanceTriangle L = LCH_MeshLightInstanceTriangleBuffer[Element.Offset()];
                        Weight = EstimateLightContribution(L, WorldPosition, WorldNormal, !bSurface);
                        Hash = L.Hash;
                    } else {
                        MeshLightInstanceClusterHeader Cluster = LCH_MeshLightInstanceClusterHeaderBuffer[Element.Offset()];
                        Weight = EstimateLightContribution(Cluster, WorldPosition, WorldNormal, !bSurface);
                        Hash = Cluster.Hash;
                    }
                    // Estimate history visibility weight
                    float VisibilityWeight = LightGrid_GridLightVisibilityWeight(GridVisibility, Hash);
                    // FIXME
                    VisibilityWeight = 1.f;
                    Weight *= VisibilityWeight;
                    if (Weight > 0.f) {
                        // Add the light to the sampler
                        LightSampler_AddListLightToSampler(LS, Weight, Element);
                        NumNonZeroGridLights ++;
                    }
                    LightListIndex++;
                }
                Iteration++;
            }
        }
    }
    // Specially, handle environment light
    LightGrid_CubicVisibility GridCubicVisibility = (LightGrid_CubicVisibility)0;
    if (bInsideLightGridBounds) {
        GridCubicVisibility = LightGrid_FetchEnvironmentVisibility(GridIndex1);
    }
    if (bWithEnvironment) {
        // Sample a certain LOD for hemispherical radiance estimation.
        float3 AvgRadiance = EvaluateEnvironmentMap_Raw(-WorldNormal, 
            LightStructure_UB.EnvironmentLightHemisphereSampleLOD, 
            LightStructure_UB.EnvironmentLightMultiplier
        );
        float Weight = EstimateEnvironmentLightContribution(AvgRadiance, WorldPosition, WorldNormal);
        uint VisibilityMask = 0;
        [unroll(LIGHT_GRID_NUM_HISTORY_FRAMES)]
        for (uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
            uint HistoryVisible = GridCubicVisibility.GridCubicHistory[i];
            // Coarse visibility condition: if any face in the direction is visible in history, it is visible
            VisibilityMask |= HistoryVisible;
        }
        // Estimate ambient occlusion
        float VisibilityFactor = 0.f;
        {
            float X = abs(WorldNormal.x) * countbits((WorldNormal.x > 0 ? 0xF0AACC : 0x0F5533) & VisibilityMask);
            float Y = abs(WorldNormal.y) * countbits((WorldNormal.y > 0 ? 0xCCF0AA : 0x330F55) & VisibilityMask);
            float Z = abs(WorldNormal.z) * countbits((WorldNormal.z > 0 ? 0xAACCF0 : 0x55330F) & VisibilityMask);
            VisibilityFactor += (X + Y + Z) / (4 * 6 * dot(abs(WorldNormal), 1.f.xxx));
        }
        // the multipler 2 is for cancelling out the duplicated normal weight from EstimateEnvironmentLightContribution
        Weight *= max(saturate(2 * VisibilityFactor), 0.1f);
        if(Weight > 0.f) {
            LightSamplerLight LSL = (LightSamplerLight)0;
            LSL.bIsEnvironment = true;
            LSL.bIsDirectional = false;
            LSL.bValid = true;
            LightSampler_AddLightToSampler(LS, Weight, LSL);
        }
    }

    // Handle the directional light
    if (bWithDirectional && DirectionalLightEnabled()) {
        float3 Direction = GetDirectionalLightDirection();
        float Weight = RadianceToLuminance(GetDirectionalLightIrradiance());
        if (bSurface) {
            Weight *= saturate(dot(WorldNormal, Direction));
        }
        if (Weight > 0.f) {
            LightSampler_AddDirectionalLightToSampler(LS, Weight);
        }
    }

    // Resample the candidates
    float SumTargetWeigts = 0.f;
    float U = R.rand();
    LightSample ReservedSample = (LightSample)0;
    // Simply assume all volumes have the same isotropic parameter g
    float g = 0.f;
    // Spawn 1 sample for each light, and resample from the samples
    for (int SamplerLightListIndex = 0; SamplerLightListIndex < NUM_LIGHT_SAMPLER_SAMPLES; SamplerLightListIndex++) {
        LightSamplerLight LSL = UnpackLightSamplerLight(LS.PackedSamplerLights[SamplerLightListIndex]);
        if(LSL.bValid) {
            float  u1 = R.rand();
            float2 u2 = R.rand2();
            LightSample Sample;
            if(bWithEnvironment && LSL.bIsEnvironment) {
                // Sample environment light
                Sample = SampleEnvironmentLightDiffuseWithPreMultiplied(
                    WorldNormal, ViewDirection,
                    bSurface,
                    g, u2
                );
                // Mark as environment light
                Sample.LightRecord = MakeEnvironmentLightSampleRecord();
            } else if (bWithDirectional && LSL.bIsDirectional) {
                Sample = SampleDirectionalLightDiffuseWithPreMultiplied(
                    WorldNormal, ViewDirection,
                    bSurface,
                    g
                );
                Sample.LightRecord = MakeDirectionalLightSampleRecord();
            } else {
                // Sample area light    
                MeshLightInstanceElementOffset Element = LSL.AbsElementIndex;
                float TreePdf;
                EvaluatedAreaLight Evaluated = LCH_SampleAndEvaluateLight(
                    Element, u1, TreePdf
                );
                Sample = SampleAreaLightDiffuseWithPreMultiplied(
                    WorldPosition, WorldNormal, ViewDirection, Evaluated, bSurface, g, u2
                );
                // Contribution from the tree traversal sampling process
                Sample.Pdf *= TreePdf;
                // Account for overflowing lights that have not been injected into the grid.
                if(LightGridLightListCdf > 0) Sample.Pdf *= LightGridLightListCdf;
                // Keep the light index
                Sample.LightRecord = MakeAreaLightSampleRecord(Element);
            }
            // 25.10.22: This must be placed OUTSIDE for unbiased normalization weight! 
            NumValidSamples ++;
            // Clip samples with low pdf (to evade potential fireflies)
            if (Sample.IsValid() && dot(Sample.Radiance, 1.f.xxx) > 0 && Sample.Pdf > 0.001f) {
                float LightCdf =  LS.Weights[SamplerLightListIndex] / LS.SumWeight;
                // Pdf of the proposed unnormalized distribution (projected solid angle for all lights,
                // a special integration domain different from both area and solid angle)
                float ProposedPdf = LightCdf * Sample.Pdf;
                // Target pdf (luminance on the hemisphere)
                float TargetPdfUnnormalized = RadianceToLuminance(Sample.Radiance);
                // RIS
                float ResampleWeight = TargetPdfUnnormalized / max(ProposedPdf, 1e-7f);
                if (ResampleWeight > 1e-4f) {
                    float CurrentLightU = ResampleWeight / (SumResampleWeights + ResampleWeight);
                    SumResampleWeights += ResampleWeight;
                    if (CurrentLightU > U) {
                        U /= CurrentLightU;
                        ReservedSample = Sample;
                    } else {
                        U = (U - CurrentLightU) / max(1 - CurrentLightU, 1e-7f);
                    }
                }
            }
        }
    }
    // Estimate the radiance using RIS.
    float Norm = SumResampleWeights / max(NumValidSamples, 1u);
    RadianceEstimation = Norm * ReservedSample.Radiance / (RadianceToLuminance(ReservedSample.Radiance) + 1e-6f);
    return ReservedSample;
}

// Set bWaveOp to true if you assume locality when accessing the light grid for each wave for better
// performance
void LightGrid_UpdateVisibilityForLightRecord(
    float3 WorldPosition, float3 WorldDirection,
    LightSampleSrcLightRecord LightRecord, bool bWaveOp = false
) {
    uint4 GridIndex = LightGrid_GetGridIndex(WorldPosition);
    if (!IsValid(GridIndex.x)) {
        return;
    }
    uint GridIndex1 = LightGrid_GetGridIndex1(GridIndex);
    if(LightRecord.IsEnvironmentLight()) {
        uint BitIndex = LightGrid_GetInfiniteVisibilityBitIndex(WorldDirection);
        InterlockedOr(
            LightGrid_RWNextEnvironmentVisibilityBuffer[GridIndex1],
            1u << BitIndex
        );
    } else if(LightRecord.IsDirectionalLight()) {
        // TODO
    } else if(LightRecord.IsMeshLightTriangle() || LightRecord.IsMeshLightCluster()) {
        uint LightIndex = LightRecord.Index();
        uint Hash32 = 0;
        if(LightRecord.IsMeshLightTriangle()) {
            MeshLightInstanceTriangle Triangle = LCH_MeshLightInstanceTriangleBuffer[LightIndex];
            Hash32 = GetLightHash32(Triangle);
        } else {
            MeshLightInstanceClusterHeader Cluster = LCH_MeshLightInstanceClusterHeaderBuffer[LightIndex];
            Hash32 = GetLightHash32(Cluster);
        }
        uint2 Hash64 = GetExpandedLightHash64(Hash32);
        if(bWaveOp) {
            bool bWaveUniform = WaveActiveAllEqual(GridIndex1);
            if(bWaveUniform) {
                Hash64 = WaveActiveBitOr(Hash64);
                if(WaveIsFirstLane()) {
                    InterlockedOr(LightGrid_RWNextBloomFilterBuffer[GridIndex1].x, Hash64.x);
                    InterlockedOr(LightGrid_RWNextBloomFilterBuffer[GridIndex1].y, Hash64.y);
                }
            } else {
                uint Iter = 0;
                while(Iter < 256) {
                    uint WaveMinGridIndex1 = WaveActiveMin(GridIndex1);
                    if(WaveMinGridIndex1 == INVALID_UINT) {
                        break;
                    }
                    if(WaveMinGridIndex1 == GridIndex1) {
                        Hash64 = WaveActiveBitOr(Hash64);
                        if(WaveIsFirstLane()) {
                            InterlockedOr(LightGrid_RWNextBloomFilterBuffer[GridIndex1].x, Hash64.x);
                            InterlockedOr(LightGrid_RWNextBloomFilterBuffer[GridIndex1].y, Hash64.y);
                        }
                        GridIndex1 = INVALID_UINT; // Invalidate
                    }
                    Iter ++;
                }
            }
        } else {
            InterlockedOr(LightGrid_RWNextBloomFilterBuffer[GridIndex1].x, Hash64.x);
            InterlockedOr(LightGrid_RWNextBloomFilterBuffer[GridIndex1].y, Hash64.y);
        }
    }
}

#endif // LIGHT_GRID_SAMPLING_HLSL
