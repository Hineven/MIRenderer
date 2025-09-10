#ifndef LIGHT_GRID_SAMPLING_HLSL
#define LIGHT_GRID_SAMPLING_HLSL

#include "LightGrid.hlsl"
#include "../headers/Scattering.hlsl"

struct LightSampler {
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
    Result.Radiance = EvaluatedEmission * saturate(Cosine) * saturate(ReceiverCosine);
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
    Result.Radiance = EvaluatedEmission * saturate(Cosine) * HenyeyGreensteinPhaseFunction(ReceiverCosine, g);
    return Result;
}

#endif // LIGHT_GRID_SAMPLING_HLSL