#ifndef MATERIAL_EVALUATION_HLSL
#define MATERIAL_EVALUATION_HLSL

#include "Transform.hlsl"
#include "Scattering.hlsl"
#include "Material.hlsl"

// FIXME non diffuse bsdf seems broken
#define DIFFUSE_BSDF

float3 EvaluateBSDF (ShadingMaterial M, float3 ViewDirection, float3 OutgoingDirection) {
    float3 LambertianTerm = EvaluateLambert(M.Albedo);
    float3 HalfVector = normalize(OutgoingDirection + ViewDirection);
    float  DotHV = dot(HalfVector, ViewDirection);
    float  DotNH = dot(M.Normal, HalfVector);
    float  DotNL = dot(M.Normal, OutgoingDirection);
    float  DotNV = dot(M.Normal, ViewDirection);
    float3 F0 = 0.04f; /* F0 for many dieletrics*/
    float3 F = 0;
    float  RoughnessAlpha = M.Roughness * M.Roughness;
    float3 Specular = EvaluateGGX(RoughnessAlpha, RoughnessAlpha * RoughnessAlpha, F0, DotHV, DotNH, DotNL, DotNV, F);
    float3 Diffuse = DiffuseCompensationTerm(F, DotHV) * LambertianTerm;
#ifdef DIFFUSE_BSDF
    return LambertianTerm;
#else
    return Specular + Diffuse;
#endif
}

float SampleBDSF (ShadingMaterial M, float3 ViewDirection, float2 U, out float3 SampledDirection) {
#ifdef DIFFUSE_BSDF
    float DiffuseProbability = 1.f;
#else
    // TODO heuristic probability
    float DiffuseProbability = 0.5f;
#endif
    float3 Tangent, Bitangent;
    GetOrthoVectors(M.Normal, Tangent, Bitangent);
    float3 LocalView = float3(
        dot(ViewDirection, Tangent),
        dot(ViewDirection, Bitangent),
        dot(ViewDirection, M.Normal)
    );
    float RoughnessAlpha = M.Roughness * M.Roughness;
    if(U.x <= DiffuseProbability) {
        U.x = U.x / DiffuseProbability;
        SampledDirection = SampleLambert(M.Albedo, U);
    } else {
        U.x = (U.x - DiffuseProbability) / (1.0f - DiffuseProbability);
        SampledDirection = SampleGGX(RoughnessAlpha, LocalView, U);
    }
    SampledDirection = SampledDirection.x * Tangent + SampledDirection.y * Bitangent + SampledDirection.z * M.Normal;
    float DotNL = dot(M.Normal, SampledDirection);
    float3 HalfVector = normalize(SampledDirection + ViewDirection);
    float  DotNH = dot(HalfVector, M.Normal);
    float  DotNV = dot(HalfVector, ViewDirection);
    float Pdf = SampleLambertPDF(DotNL) * DiffuseProbability
     + SampleGGXPDF(RoughnessAlpha * RoughnessAlpha, DotNH, DotNV, LocalView) * (1.0f - DiffuseProbability);
    return Pdf;
}

// Evaluate cached material brdf
// Simple lambertian
float3 EvaluateCachedMaterialBRDF (
    CachedHitMaterial M, float3 Normal, float3 ViewDirection, float3 OutgoingDirection,
    float PhaseG
) {
    if(M.bIsSurface) {
        if(dot(Normal, ViewDirection) * dot(Normal, OutgoingDirection) <= 0) return 0;
        return EvaluateLambert(M.Albedo);
    } else {
        float Cosine = dot(Normal, ViewDirection);
        return HenyeyGreensteinPhaseFunction(Cosine, PhaseG) * M.Albedo;
    }
}

#endif