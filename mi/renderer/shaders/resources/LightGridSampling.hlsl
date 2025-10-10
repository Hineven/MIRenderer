#ifndef LIGHT_GRID_SAMPLING_HLSL
#define LIGHT_GRID_SAMPLING_HLSL

#include "LightGrid.hlsl"
#include "../headers/Random.hlsl"
#include "../headers/Scattering.hlsl"
#include "../resources/LightEvaluation.hlsl"

#ifndef NUM_LIGHT_SAMPELR_SAMPLES
#define NUM_LIGHT_SAMPELR_SAMPLES 6
// #error NUM_LIGHT_SAMPELR_SAMPLES must be defined for this header
#endif

StructuredBuffer<AreaLight> LightBuffer;

StructuredBuffer<uint> ActiveLightListCount;
StructuredBuffer<uint> ActiveLightListBuffer;

StructuredBuffer<uint> LightGrid_ListLightIndexBuffer;
StructuredBuffer<uint> LightGrid_GridLightListOffsetBuffer;
StructuredBuffer<float> LightGrid_GridLightListCdfBuffer;
StructuredBuffer<uint> LightGrid_GridLightListLengthBuffer;
// Record the combination of light encodings that successfully illuminated geometries in the grid
StructuredBuffer<uint4> LightGrid_BloomFilterBuffer;

StructuredBuffer<PackedPrecomputedLight> PrecomputedActiveLightBuffer;

struct LightSampler {
    uint NumResampledLights;
    float SumWeight;
    float SampleU[NUM_LIGHT_SAMPELR_SAMPLES];
    float Weights[NUM_LIGHT_SAMPELR_SAMPLES];
    uint ActiveLightListIndex[NUM_LIGHT_SAMPELR_SAMPLES];
};

LightSampler InitLightSampler(inout Random R) {
    LightSampler LS = (LightSampler)0;
    // Scatter samples
    for (int i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        float Step = (1.f / NUM_LIGHT_SAMPELR_SAMPLES);
        LS.SampleU[i] = saturateDown((R.rand() + i) * Step);
        LS.ActiveLightListIndex[i] = INVALID_UINT;
        LS.Weights[i] = 0.f;
    }
    return LS;
}

void AddLightToSampler(inout LightSampler LS, float Weight, uint Index) {
    float U = Weight / (LS.SumWeight + Weight + 1e-6f);
    LS.NumResampledLights ++;
    LS.SumWeight += Weight;
    for (uint i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        bool bSelect = false;
        if (U > LS.SampleU[i]) bSelect = true;
        if (bSelect) {
            LS.ActiveLightListIndex[i] = Index;
            LS.SampleU[i] = LS.SampleU[i] / U;
            LS.Weights[i] = Weight;
        } else {
            LS.SampleU[i] = saturateDown((LS.SampleU[i] - U) / (1.f - U));
        }
    }
}

struct LightSample {
    uint Index;
    float3 Position;
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

LightSample SampleLightDiffuseWithPreMultipliedCosine(float3 Position, float3 Normal, EvaluatedLight Evaluated, float2 u2) {
    LightSample Result = (LightSample)0;
    float Area = 0.f;
    Result.Position = SampleAreaLightArea(Evaluated.V0, Evaluated.V1, Evaluated.V2, u2, Area, Result.Pdf);
    float2 UV = InterpolateBarycentrics(Evaluated.UV0, Evaluated.UV1, Evaluated.UV2, u2);
    // Convert area pdf to solid angle pdf
    float Distance = length(Result.Position - Position);
    float3 Direction = normalize(Result.Position - Position);
    float3 LightNormal = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    float Cosine = dot(LightNormal, -Direction);
    float ReceiverCosine = dot(Direction, Normal);
    Result.Pdf *= Distance * Distance / max(abs(Cosine), 1e-4f);
    float3 EvaluatedEmission = Evaluated.Emission;
    if (IsValid(Evaluated.EmissionTextureIndex))
        EvaluatedEmission += GetBindlessSRV(Evaluated.EmissionTextureIndex).SampleLevel(LinearWrapSampler, UV, 0).rgb;
    Result.Radiance = EvaluatedEmission * saturate(ReceiverCosine);
    return Result;
}

LightSample SampleLightWithPreMultipliedPhaseFunction(
    float3 Position, float3 ViewDirection, EvaluatedLight Evaluated, float g, float2 u2
) {
    LightSample Result = (LightSample)0;
    float Area = 0.f;
    Result.Position = SampleAreaLightArea(Evaluated.V0, Evaluated.V1, Evaluated.V2, u2, Area, Result.Pdf);
    float2 UV = InterpolateBarycentrics(Evaluated.UV0, Evaluated.UV1, Evaluated.UV2, u2);
    // Convert area pdf to solid angle pdf
    float Distance = length(Result.Position - Position);
    float3 Direction = normalize(Result.Position - Position);
    float3 LightNormal = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    float Cosine = dot(LightNormal, -Direction);
    float ReceiverCosine = dot(Direction, ViewDirection);
    Result.Pdf *= Distance * Distance / max(abs(Cosine), 1e-4f);
    float3 EvaluatedEmission = Evaluated.Emission;
    if (IsValid(Evaluated.EmissionTextureIndex))
        EvaluatedEmission += GetBindlessSRV(Evaluated.EmissionTextureIndex).SampleLevel(LinearWrapSampler, UV, 0).rgb;
    Result.Radiance = EvaluatedEmission * HenyeyGreensteinPhaseFunction(ReceiverCosine, g);
    return Result;
}

LightSample SampleOneLightSample_RIS (
    float3 WorldPosition, float3 WorldNormal, float3 ViewDirection,
    bool bSurface, bool bGroupedAccess,
    inout Random R, out float3 SumResampleWeights3, out uint NumValidSamples,
    out float LightGridLightListCdf
) {
    NumValidSamples = 0;
    SumResampleWeights3 = 0.f;
    LightGridLightListCdf = 1.f;
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

    uint NumNonZeroGridLights = 0;
    bool bUniformGrid = WaveActiveAllEqual(GridIndex1);
    if (bUniformGrid || !bGroupedAccess) {
        // Assume one wave have locality regarding the grid index
        for (uint LightListIndex = 0; LightListIndex < NumGridLights; LightListIndex++) {
            uint ActiveLightListIndex = LightGrid_ListLightIndexBuffer[GridLightListOffset + LightListIndex];
            PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[ActiveLightListIndex]);
            float Weight = EstimateLightContribution(L, WorldPosition, WorldNormal);
            if(Weight > 0.f) {
                AddLightToSampler(LS, Weight, ActiveLightListIndex);
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
            ActiveLightListIndex = LightGrid_ListLightIndexBuffer[GridLightListOffset + LightListIndex];
            uint WaveMinLightIndex = WaveActiveMin(ActiveLightListIndex);
            if (WaveMinLightIndex == ActiveLightListIndex) {
                PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[ActiveLightListIndex]);
                float Weight = EstimateLightContribution(L, WorldPosition, WorldNormal);
                if (Weight > 0.f) {
                    // Add the light to the sampler
                    AddLightToSampler(LS, Weight, ActiveLightListIndex);
                    NumNonZeroGridLights ++;
                }
                LightListIndex++;
            }
            Iteration++;
        }
    }
    float SumResampleWeights = 0.f, SumTargetWeigts = 0.f;
    float U = R.rand();
    LightSample ReservedSample = (LightSample)0;
    // Simply assume all volumes have the same isotropic parameter g
    float g = 0.f;
    // Spawn 1 sample for each light, and resample from the samples
    for (int SamplerLightListIndex = 0; SamplerLightListIndex < NUM_LIGHT_SAMPELR_SAMPLES; SamplerLightListIndex++) {
        uint ActiveLightListIndex = LS.ActiveLightListIndex[SamplerLightListIndex];
        if(IsValid(ActiveLightListIndex)) {
            uint LightIndex = ActiveLightListBuffer[ActiveLightListIndex];
            EvaluatedLight Evaluated = EvaluateLight(LightBuffer[LightIndex]);
            float2 u2 = R.rand2();
            LightSample Sample;
            if(bSurface) {
                Sample = SampleLightDiffuseWithPreMultipliedCosine(WorldPosition, WorldNormal, Evaluated, u2);
            } else {
                Sample = SampleLightWithPreMultipliedPhaseFunction(
                WorldPosition, ViewDirection, Evaluated, g, u2
            );
            }
            // Clip samples with low pdf (potential fireflies)
            if (Sample.IsValid() && Sample.Pdf > 0.001f) {
                NumValidSamples ++;
                float LightCdf =  LS.Weights[SamplerLightListIndex] / LS.SumWeight;
                // Pdf of the proposal distribution (hemisphere)
                // TODO : why not divide Pdf??
                float ProposedPdf = LightCdf;// * Sample.Pdf;
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