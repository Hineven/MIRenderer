#ifndef LIGHT_GRID_SAMPLING_HLSL
#define LIGHT_GRID_SAMPLING_HLSL

#include "LightGrid.hlsl"
#include "../headers/Random.hlsl"
#include "../headers/Scattering.hlsl"
#include "../headers/OctahedronMapping.hlsl"
#include "../resources/LightEvaluation.hlsl"

#ifndef NUM_LIGHT_SAMPELR_SAMPLES
#define NUM_LIGHT_SAMPELR_SAMPLES 6
// #error NUM_LIGHT_SAMPELR_SAMPLES must be defined for this header
#endif

StructuredBuffer<AreaLight> LightBuffer;

RWStructuredBuffer<uint> LightGrid_ActiveLightListCount;
RWStructuredBuffer<uint> LightGrid_ActiveLightListBuffer;

RWStructuredBuffer<uint>  LightGrid_ListAllocator;
RWStructuredBuffer<uint>  LightGrid_ListActiveLightListIndexBuffer;
RWStructuredBuffer<uint> LightGrid_GridLightListOffsetBuffer;
RWStructuredBuffer<float> LightGrid_GridLightListCdfBuffer;
RWStructuredBuffer<uint> LightGrid_GridLightListLengthBuffer;
// Record the combination of light encodings that successfully illuminated geometries in the grid
RWStructuredBuffer<uint4> LightGrid_BloomFilterBuffer;
// Record the importance of environment light grids (8x8 = 64 octahedral grids, 2 frames history)
RWStructuredBuffer<uint64_t> LightGrid_EnvironmentVisibilityHistoryBuffer;

RWStructuredBuffer<PackedPrecomputedLight> LightGrid_PrecomputedActiveLightBuffer;

struct LightSampler {
    uint NumResampledLights;
    float SumWeight;
    float SampleU[NUM_LIGHT_SAMPELR_SAMPLES];
    float Weights[NUM_LIGHT_SAMPELR_SAMPLES];
    uint  PackedSamplerLights[NUM_LIGHT_SAMPELR_SAMPLES];
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
    for (int i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        float Step = (1.f / NUM_LIGHT_SAMPELR_SAMPLES);
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
    for (uint i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
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
    uint Index;
    // For area light: sampled position on the light
    // For environment light: sampled normalized direction
    float3 Position;
    // Pdf in solid angle domain
    float Pdf;
    float3 Radiance;
    bool IsValid() {
        return Pdf > 0;
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
    float Cosine = dot(LightNormal, -Direction);
    float ReceiverCosine = bSurface ? dot(Direction, Normal) : dot(Direction, ViewDirection);
    // Convert surface domain pdf to solid angle domain pdf
    Result.Pdf *= Distance * Distance / max(abs(Cosine), 1e-4f);
    float3 EvaluatedEmission = Evaluated.Emission;
    if (IsValid(Evaluated.EmissionTextureIndex))
        EvaluatedEmission += GetBindlessSRV(Evaluated.EmissionTextureIndex).SampleLevel(LinearWrapSampler, UV, 0).rgb;
    float PreMultiplied = bSurface ? saturate(ReceiverCosine) : HenyeyGreensteinPhaseFunction(ReceiverCosine, g);
    Result.Radiance = EvaluatedEmission * PreMultiplied;
    return Result;
}

// Importance sample a light with history info from the environment light with pre-multiplied cosine / phase function
LightSample ImportanceSampleEnvironmentLightWithPreMultiplied(
    float3 Normal, float3 ViewDirection,
    bool bSurface,
    float g, float2 u2
) {
    // TODO
    asdfasdfasd
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

#define LIGHT_GRID_NUM_HISTORY_FRAMES 2

struct LightGrid_SphericalVisibility {
    uint2 GridEnvironmentHistory[LIGHT_GRID_NUM_HISTORY_FRAMES];
    // TODO history for other lights
};

LightGrid_SphericalVisibility FetchLightGridSphericalVisibility(uint GridIndex1) {
    LightGrid_SphericalVisibility History = (LightGrid_SphericalVisibility)0;
    for (uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
        History.GridEnvironmentHistory[i] = LightGrid_EnvironmentVisibilityHistoryBuffer[GridIndex1 * LIGHT_GRID_NUM_HISTORY_FRAMES + i];
    }
    return History;
}

uint FindNthSetBit (uint64_t LongCandidateBitMask, uint SetBitRank) {
    // Find n-th set bit
    // Theoretically NVIDIA hardware has builtin __fns instruction to accelerate this.
    // However i found no existing extension in SPIRV to expose that instruction. So we fall back to a loop here.
    uint LowBitCount = countbits(uint(LongCandidateBitMask & 0xFFFFFFFFull));
    uint CandidateMask = uint(LongCandidateBitMask & 0xFFFFFFFFull);
    uint OutRank = 0;
    if(LowBitCount < SetBitRank) {
        OutRank += 32;
        SetBitRank -= LowBitCount;
        CandidateMask = uint(LongCandidateBitMask >> 32);
    }
    uint Mask = 0xFFFF;
    for(int i = 0; i < 5; i++) {
        uint BitCount = countbits(CandidateMask & Mask);
        if(BitCount < SetBitRank) {
            OutRank += (16 >> i);
            SetBitRank -= BitCount;
            CandidateMask = CandidateMask >> (16 >> i);
        }
        Mask = Mask >> (8 >> i);
    }
    return OutRank;
}

// uint SampleEnvironmentLightTileIndex (uint GridIndex1, float3 Normal, bool bSurface, out float Cdf) {
//     Cdf = 0.f;
//     LightGridHistory History = FetchLightGridHistory(GridIndex1);
//     // Importance sampling the environment based on visibility history
//     // Not visible at all: 0.2 weight
//     // Semi visible: 0.5 weight
//     // All visible: 1.0 weight
//     uint64_t NotVisibleMask = 0;
//     uint64_t SemiOrAllVisibleMask = 0;
//     uint64_t AllVisibleMask = 0xFFFFFFFFFFFFFFFFull;
//     for (uint i = 0; i < LIGHT_GRID_NUM_HISTORY_FRAMES; i++) {
//         NotVisibleMask |= ~History.GridEnvironmentHistory[i];
//         SemiOrAllVisibleMask |= History.GridEnvironmentHistory[i];
//         AllVisibleMask &= History.GridEnvironmentHistory[i];
//     }
//     uint2 SemiVisibleMask = SemiOrAllVisibleMask & ~AllVisibleMask;
//     uint NotVisibleCount  = countbits(NotVisibleMask);
//     uint SemiVisibleCount = countbits(SemiVisibleMask);
//     uint AllVisibleCount  = countbits(AllVisibleMask);
//     float NotVisibleWeight  = NotVisibleCount * 0.2f;
//     float SemiVisibleWeight = SemiVisibleCount * 0.5f;
//     float AllVisibleWeight  = AllVisibleCount * 1.0f;
//     float TotalWeight = NotVisibleWeight + SemiVisibleWeight + AllVisibleWeight;
//     if (TotalWeight > 0.f) {
//         // Importance sample the environment light
//         float U = R.rand() * TotalWeight;
//         uint64_t LongCandidateMask = 0;
//         if(U < NotVisibleWeight) {
//             LongCandidateMask = NotVisibleMask;
//             U = U / NotVisibleWeight;
//             Cdf = 3 * NotVisibleWeight / TotalWeight;
//         } else if (U < NotVisibleWeight + SemiVisibleWeight) {
//             LongCandidateMask = SemiVisibleMask;
//             U = (U - NotVisibleWeight) / SemiVisibleWeight;
//             Cdf = 3 * SemiVisibleWeight / TotalWeight;
//         } else {
//             LongCandidateMask = AllVisibleMask;
//             U = (U - NotVisibleWeight - SemiVisibleWeight) / AllVisibleWeight;
//             Cdf = 3 * AllVisibleWeight / TotalWeight;
//         }
//         // Select one visible direction
//         uint SetBitRank = (uint)(U * countbits(LongCandidateMask));
//         uint OutRank = 0;
//         uint BitRank = FindNthSetBit(LongCandidateMask, SetBitRank);
//         return BitRank;
//     }
//     return INVALID_UINT;
// }

// bSurface: if the sample is sampled for surface shading (otherwise we assume volume shading and WorldNormal is omitted)
// bGroupedAccess: whether locality is assumed when accessing the light grid for each wave. You can enable this when you 
// know that the threads in a wave will access similar grid cells (e.g., tiled rendering on screen)
// bWithEnvironment: whether environment light is considered and can be sampled
LightSample SampleOneLightSample_RIS (
    float3 WorldPosition, float3 WorldNormal, float3 ViewDirection,
    bool bSurface, bool bGroupedAccess, bool bWithEnvironment,
    inout Random R, out float3 SumResampleWeights3, out uint NumValidSamples,
    out float LightGridLightListCdf
) {
    NumValidSamples = 0;
    SumResampleWeights3 = 0.f;
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

    // Spawn candidate samples from the lights in the grid
    uint NumNonZeroGridLights = 0;
    bool bUniformGrid = WaveActiveAllEqual(GridIndex1);
    if (bUniformGrid || !bGroupedAccess) {
        // Assume one wave have locality regarding the grid index
        for (uint LightListIndex = 0; LightListIndex < NumGridLights; LightListIndex++) {
            uint ActiveLightListIndex = LightGrid_ListActiveLightListIndexBuffer[GridLightListOffset + LightListIndex];
            PrecomputedLight L = UnpackPrecomputedLight(LightGrid_PrecomputedActiveLightBuffer[ActiveLightListIndex]);
            float Weight = EstimateLightContribution(L, WorldPosition, WorldNormal);
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
    LightGrid_SphericalVisibility GridSphericalVisibility = LightGrid_FetchEnvironmentVisibility(GridIndex1);
    if (bWithEnvironment) {
        float Weight = EstimateEnvironmentLightContribution(GridSphericalVisibility, WorldPosition, WorldNormal);
        if(Weight > 0.f) {
            LightSamplerLight LSL = (LightSamplerLight)0;
            LSL.bIsEnvironment = true;
            LightSampler_AddLightToSampler(LS, Weight, LSL);
        }
    }

    // Resample the candidates
    float SumResampleWeights = 0.f, SumTargetWeigts = 0.f;
    float U = R.rand();
    LightSample ReservedSample = (LightSample)0;
    // Simply assume all volumes have the same isotropic parameter g
    float g = 0.f;
    // Spawn 1 sample for each light, and resample from the samples
    for (int SamplerLightListIndex = 0; SamplerLightListIndex < NUM_LIGHT_SAMPELR_SAMPLES; SamplerLightListIndex++) {
        LightSamplerLight LSL = UnpackLightSamplerLight(LS.PackedSamplerLights[SamplerLightListIndex]);
        if(LSL.bValid) {
            float2 u2 = R.rand2();
            LightSample Sample;
            if(LSL.bIsEnvironment) {
                // Sample sub environment light
                uint TileIndex = LSL.ActiveLightListIndex;
                Sample = ImportanceSampleEnvironmentLightWithPreMultiplied(
                    WorldNormal, ViewDirection,
                    bSurface,
                    g, u2
                );
            } else {         
                uint ActiveLightListIndex = LSL.ActiveLightListIndex;
                uint LightIndex = LightGrid_ActiveLightListBuffer[ActiveLightListIndex];
                EvaluatedAreaLight Evaluated = EvaluateLight(LightBuffer[LightIndex]);
                Sample = SampleAreaLightDiffuseWithPreMultiplied(WorldPosition, WorldNormal, ViewDirection, Evaluated, bSurface, g, u2);
            }
            // Clip samples with low pdf (potential fireflies)
            if (Sample.IsValid() && Sample.Pdf > 0.005f) {
                NumValidSamples ++;
                float LightCdf =  LS.Weights[SamplerLightListIndex] / LS.SumWeight;
                // Pdf of the proposal unnormalized distribution (hemisphere)
                float ProposedPdf = LightCdf;
                // Target pdf (light contribution)
                float3 TargetPdf3Unnormalized = Sample.Radiance / Sample.Pdf;
                float TargetPdfUnnormalized = dot(TargetPdf3Unnormalized, 1.f.xxx);
                // RIS
                float3 ResampleWeight3 = TargetPdf3Unnormalized / ProposedPdf;
                float ResampleWeight = TargetPdfUnnormalized / max(ProposedPdf, 1e-7f);
                if (ResampleWeight > 1e-4f) {
                    float CurrentLightU = ResampleWeight / (SumResampleWeights + ResampleWeight);
                    SumResampleWeights3 += ResampleWeight3;
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
    return ReservedSample;
}

#endif // LIGHT_GRID_SAMPLING_HLSL