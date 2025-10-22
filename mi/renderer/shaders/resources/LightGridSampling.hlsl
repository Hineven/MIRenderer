#ifndef LIGHT_GRID_SAMPLING_HLSL
#define LIGHT_GRID_SAMPLING_HLSL
#include "../headers/Random.hlsl"
#include "../headers/Scattering.hlsl"
#include "../headers/OctahedronMapping.hlsl"
#include "../headers/Sampling.hlsl"
#include "../headers/Scattering.hlsl"
#include "../resources/LightEvaluation.hlsl"

#include "LightGrid.hlsl"
#include "EnvironmentLightResource.hlsl"

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

StructuredBuffer<AreaLight> LightBuffer;

RWStructuredBuffer<uint> LightGrid_ActiveLightListCount;
RWStructuredBuffer<uint> LightGrid_ActiveLightListBuffer;

RWStructuredBuffer<uint>  LightGrid_ListAllocator;
RWStructuredBuffer<uint>  LightGrid_ListActiveLightListIndexBuffer;
RWStructuredBuffer<uint> LightGrid_GridLightListOffsetBuffer;
RWStructuredBuffer<float> LightGrid_GridLightListCdfBuffer;
RWStructuredBuffer<uint> LightGrid_GridLightListLengthBuffer;
// Record the importance of environment light grids (2x2x6 cubic tiles)
// (low end) | +x | -x | +y | -y | +z | -z | (high end) 
// for each individual face with axis t, p = t+1, q = t+2, the 4 bits are arranged as:
// 0: (p+, q+), 1: (p+, q-), 2: (p-, q+), 3: (p-, q-) 
RWStructuredBuffer<uint> LightGrid_EnvironmentVisibilityHistoryBuffer;
// Record the combination of light encodings that successfully illuminated geometries in the grid
// 64 bits per grid cell per frame
RWStructuredBuffer<uint2> LightGrid_BloomFilterBuffer;

RWStructuredBuffer<uint2> LightGrid_NextBloomFilterBuffer;
RWStructuredBuffer<uint>  LightGrid_NextEnvironmentVisibilityBuffer;

RWStructuredBuffer<PackedPrecomputedLight> LightGrid_PrecomputedActiveLightBuffer;


struct LightSampler {
    uint NumResampledLights;
    float SumWeight;
    float SampleU[NUM_LIGHT_SAMPLER_SAMPLES];
    float Weights[NUM_LIGHT_SAMPLER_SAMPLES];
    uint  PackedSamplerLights[NUM_LIGHT_SAMPLER_SAMPLES];
};

struct LightSamplerLight {
    uint ActiveLightListIndex; // ActiveLightListIndex or environment light octahedron tile index
    bool bIsEnvironment;
    bool bValid;
};

uint PackLightSamplerLight(LightSamplerLight LSL) {
    uint Packed = 0;
    Packed |= LSL.ActiveLightListIndex & 0x7FFFFFFF;
    Packed |= (LSL.bIsEnvironment ? 1u : 0u) << 31;
    // INVALID_UINT for invalid light
    Packed |= (LSL.bValid ? 0u : INVALID_UINT);
    return Packed;
}

LightSamplerLight UnpackLightSamplerLight(uint Packed) {
    LightSamplerLight LSL = (LightSamplerLight)0;
    LSL.ActiveLightListIndex = Packed & 0x7FFFFFFF;
    LSL.bIsEnvironment = ((Packed >> 31) & 0x1) != 0;
    LSL.bValid = (Packed != INVALID_UINT);
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

void LightSampler_AddListLightToSampler(inout LightSampler LS, float Weight, uint ActiveLightListIndex) {
    LightSamplerLight LSL = (LightSamplerLight)0;
    LSL.ActiveLightListIndex = ActiveLightListIndex;
    LSL.bIsEnvironment = false;
    LSL.bValid = true;
    LightSampler_AddLightToSampler(LS, Weight, LSL);
}

struct LightSample {
    // For area light: sampled position on the light
    // For environment light: sampled normalized direction
    float3 Position;
    // Pdf in solid angle domain
    float Pdf;
    float3 Radiance;
    // Keep the index of the sampled light. INVALID_UINT for environment light
    uint LightIndex;
    bool bIsEnvironmentLightSample;
    // Returns true if the sample is valid: pdf > 0
    bool IsValid() {
        return Pdf > 0;
    }
    bool IsEnvironmentLight() {
        return bIsEnvironmentLightSample;
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
    float PreMultipliedSaturate = saturate(ReceiverCosine);
    float PreMultipliedHG = HenyeyGreensteinPhaseFunction(ReceiverCosine, g);
    float PreMultiplied = bSurface ? PreMultipliedSaturate : PreMultipliedHG;
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
    float3 EvaluatedEmission = EvaluateEnvironmentMap(Direction).rgb;
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

struct LightGrid_CubicVisibility {
    uint GridCubicHistory[LIGHT_GRID_NUM_HISTORY_FRAMES];
};

LightGrid_CubicVisibility LightGrid_FetchEnvironmentVisibility(uint GridIndex1) {
    LightGrid_CubicVisibility Visibility = (LightGrid_CubicVisibility)0;
    [unroll(LIGHT_GRID_NUM_HISTORY_FRAMES)]
    for (uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
        Visibility.GridCubicHistory[i] = LightGrid_EnvironmentVisibilityHistoryBuffer[GridIndex1 * LIGHT_GRID_NUM_HISTORY_FRAMES + i];
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
        GridVisibility.BloomFilters[i] = LightGrid_BloomFilterBuffer[GridIndex1 * LIGHT_GRID_NUM_HISTORY_FRAMES + i];
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

// bSurface: if the sample is sampled for surface shading (otherwise we assume volume shading and WorldNormal is omitted)
// bGroupedAccess: whether locality is assumed when accessing the light grid for each wave. You can enable this when you 
// know that the threads in a wave will access similar grid cells (e.g., tiled rendering on screen)
// bWithEnvironment: whether environment light is considered and can be sampled
LightSample SampleOneLightSample_RIS (
    float3 WorldPosition, float3 WorldNormal, float3 ViewDirection,
    bool bSurface, bool bGroupedAccess, bool bWithEnvironment,
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
    if (!IsValid(GridIndex.x)) {
        return Sample; // Out of light grid
    }
    uint GridIndex1 = LightGrid_GetGridIndex1(GridIndex);

    LightSampler LS = InitLightSampler(R);

    float GridSize = 0;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    uint NumGridLights = LightGrid_GridLightListLengthBuffer[GridIndex1];
    uint GridLightListOffset = LightGrid_GridLightListOffsetBuffer[GridIndex1];
    LightGridLightListCdf = LightGrid_GridLightListCdfBuffer[GridIndex1];

    LightGrid_GridLightVisibility GridVisibility = GetGridLightVisibility(GridIndex1);

    // Spawn candidate samples from the lights in the grid
    uint NumNonZeroGridLights = 0;
    bool bUniformGrid = WaveActiveAllEqual(GridIndex1);
    if (bUniformGrid || !bGroupedAccess) {
        // Assume one wave have locality regarding the grid index
        for (uint LightListIndex = 0; LightListIndex < NumGridLights; LightListIndex++) {
            uint ActiveLightListIndex = LightGrid_ListActiveLightListIndexBuffer[GridLightListOffset + LightListIndex];
            PrecomputedLight L = UnpackPrecomputedLight(LightGrid_PrecomputedActiveLightBuffer[ActiveLightListIndex]);
            float Weight = EstimateLightContribution(L, WorldPosition, WorldNormal);
            // Estimate history visibility weight
            float VisibilityWeight = LightGrid_GridLightVisibilityWeight(GridVisibility, L.Hash);
            Weight *= VisibilityWeight;
            if(Weight > 0.f) {
                LightSampler_AddListLightToSampler(LS, Weight, ActiveLightListIndex);
                NumNonZeroGridLights ++;
            }
        }
    } else {
        // Process the light with the minimum index in the grid
        uint LightListIndex = 0, Iteration = 0;
        // TODO remove Iteration (used to prevent driver timeouts)
        // TODO add a noisy occlusion modifier based on history cache to the target distribution
        while(LightListIndex < NumGridLights && Iteration < 256) {
            uint ActiveLightListIndex = INVALID_UINT;
            ActiveLightListIndex = LightGrid_ListActiveLightListIndexBuffer[GridLightListOffset + LightListIndex];
            uint WaveMinLightIndex = WaveActiveMin(ActiveLightListIndex);
            if (WaveMinLightIndex == ActiveLightListIndex) {
                PrecomputedLight L = UnpackPrecomputedLight(LightGrid_PrecomputedActiveLightBuffer[ActiveLightListIndex]);
                float Weight = EstimateLightContribution(L, WorldPosition, WorldNormal);
                // Estimate history visibility weight
                float VisibilityWeight = LightGrid_GridLightVisibilityWeight(GridVisibility, L.Hash);
                Weight *= VisibilityWeight;
                if (Weight > 0.f) {
                    // Add the light to the sampler
                    LightSampler_AddListLightToSampler(LS, Weight, ActiveLightListIndex);
                    NumNonZeroGridLights ++;
                }
                LightListIndex++;
            }
            Iteration++;
        }
    }
    // Specially, handle environment light
    LightGrid_CubicVisibility GridCubicVisibility = LightGrid_FetchEnvironmentVisibility(GridIndex1);
    if (bWithEnvironment) {
        float3 AvgRadiance = EvaluateEnvironmentMap(-WorldNormal, LightStructure_UB.EnvironmentLightHemisphereSampleLOD);
        float Weight = EstimateEnvironmentLightContribution(AvgRadiance, WorldPosition, WorldNormal);
        uint VisibilityMask = 0;
        [unroll(LIGHT_GRID_NUM_HISTORY_FRAMES)]
        for (uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
            uint HistoryVisible = GridCubicVisibility.GridCubicHistory[i];
            // Coarse visibility condition: if any face in the direction is visible in history, consider it visible
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
        Weight *= saturate(2 * VisibilityFactor);
        if(Weight > 0.f) {
            LightSamplerLight LSL = (LightSamplerLight)0;
            LSL.bIsEnvironment = true;
            LightSampler_AddLightToSampler(LS, Weight, LSL);
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
            float2 u2 = R.rand2();
            LightSample Sample;
            if(bWithEnvironment && LSL.bIsEnvironment) {
                // Sample environment light
                uint TileIndex = LSL.ActiveLightListIndex;
                Sample = SampleEnvironmentLightDiffuseWithPreMultiplied(
                    WorldNormal, ViewDirection,
                    bSurface,
                    g, u2
                );
                // Mark as environment light
                Sample.LightIndex = INVALID_UINT;
            } else {
                // Sample area light    
                uint ActiveLightListIndex = LSL.ActiveLightListIndex;
                uint LightIndex = LightGrid_ActiveLightListBuffer[ActiveLightListIndex];
                bool bActive;
                EvaluatedAreaLight Evaluated = EvaluateLight(LightBuffer[LightIndex], bActive);
                Sample = SampleAreaLightDiffuseWithPreMultiplied(WorldPosition, WorldNormal, ViewDirection, Evaluated, bSurface, g, u2);
                // Keep the light index
                Sample.LightIndex = LightIndex;
            }
            // 25.10.22: This must be placed OUTSIDE for unbiased normalization weight! 
            NumValidSamples ++;
            // Clip samples with low pdf (to evade potential fireflies)
            if (Sample.IsValid() && dot(Sample.Radiance, 1.f.xxx) > 0 && Sample.Pdf > 0.005f) {
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
    // Account for overflowing lights that have not been injected into the grid.
    if(LightGridLightListCdf > 0) RadianceEstimation /= LightGridLightListCdf;
    else RadianceEstimation = 0;
    return ReservedSample;
}

// Set bWaveOp to true if you assume locality when accessing the light grid for each wave for better
// performance
void LightGrid_UpdateVisibilityForAreaLight(float3 WorldPosition, uint LightIndex, bool bWaveOp = false) {
    uint4 GridIndex = LightGrid_GetGridIndex(WorldPosition);
    uint GridIndex1 = LightGrid_GetGridIndex1(GridIndex);
    AreaLight LightData = LightBuffer[LightIndex];
    uint2 Hash64 = GetExpandedLightHash64(LightIndex, GetLightHash32(LightData));
    if(bWaveOp) {
        bool bWaveUniform = WaveActiveAllEqual(GridIndex1);
        if(bWaveUniform) {
            Hash64 = WaveActiveBitOr(Hash64);
            if(WaveIsFirstLane()) {
                InterlockedOr(LightGrid_NextBloomFilterBuffer[GridIndex1].x, Hash64.x);
                InterlockedOr(LightGrid_NextBloomFilterBuffer[GridIndex1].y, Hash64.y);
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
                        InterlockedOr(LightGrid_NextBloomFilterBuffer[GridIndex1].x, Hash64.x);
                        InterlockedOr(LightGrid_NextBloomFilterBuffer[GridIndex1].y, Hash64.y);
                    }
                    GridIndex1 = INVALID_UINT; // Invalidate
                }
                Iter ++;
            }
        }
    } else {
        InterlockedOr(LightGrid_NextBloomFilterBuffer[GridIndex1].x, Hash64.x);
        InterlockedOr(LightGrid_NextBloomFilterBuffer[GridIndex1].y, Hash64.y);
    }
}

void LightGrid_UpdateVisibilityForEnvironmentLight(float3 WorldPosition, float3 Direction) {
    uint4 GridIndex = LightGrid_GetGridIndex(WorldPosition);
    uint GridIndex1 = LightGrid_GetGridIndex1(GridIndex);
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
    // Sub index within the cube face
    uint2 SubIndex = select(SubDirection > 0, 0.xx, 1.xx);
    uint BitIndex = FaceIndex * 4 + SubIndex.x * 2 + SubIndex.y;
    InterlockedOr(
        LightGrid_NextEnvironmentVisibilityBuffer[GridIndex1],
        1u << BitIndex
    );
}

#endif // LIGHT_GRID_SAMPLING_HLSL