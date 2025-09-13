#ifndef MATERIAL_EVALUATION_HLSL
#define MATERIAL_EVALUATION_HLSL


#include "Transform.hlsl"
#include "Scattering.hlsl"
#include "Material.hlsl"

float3 EvaluateBSDF (ShadingMaterial M, float3 ViewDirection, float3 OutgoingDirection) {
    float3 Tangent, Bitangent;
    GetOrthoVectors(M.Normal, Tangent, Bitangent);
    float3 LocalView = float3(
        dot(ViewDirection, Tangent),
        dot(ViewDirection, Bitangent),
        dot(ViewDirection, M.Normal)
    );
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
    float3 Diffuse  = DiffuseCompensationTerm(F, DotHV) * LambertianTerm;
    return Specular + Diffuse;
}

float SampleBDSF (ShadingMaterial M, float3 ViewDirection, float2 U, out float3 SampledDirection) {
    float DiffuseProbability = 0.5f;
    float3 Tangent, Bitangent;
    GetOrthoVectors(M.Normal, Tangent, Bitangent);
    float3 LocalView = float3(
        dot(ViewDirection, Tangent),
        dot(ViewDirection, Bitangent),
        dot(ViewDirection, M.Normal)
    );
    if(U.x <= DiffuseProbability) {
        U.x = U.x / DiffuseProbability;
        SampledDirection = SampleLambert(M.Albedo, U);
    } else {
        U.x = (U.x - DiffuseProbability) / (1.0f - DiffuseProbability);
        SampledDirection = SampleGGX(M.Roughness * M.Roughness, LocalView, U);
    }
    SampledDirection = SampledDirection.x * Tangent + SampledDirection.y * Bitangent + SampledDirection.z * M.Normal;
    float DotNL = dot(M.Normal, SampledDirection);
    float RoughnessAlpha = M.Roughness * M.Roughness;
    float3 HalfVector = normalize(SampledDirection + ViewDirection);
    float  DotNH = dot(HalfVector, M.Normal);
    float  DotNV = dot(HalfVector, ViewDirection);
    float Pdf = SampleLambertPDF(DotNL) * DiffuseProbability
     + SampleGGXPDF(RoughnessAlpha * RoughnessAlpha, DotNH, DotNV, LocalView) * (1.0f - DiffuseProbability);
    return Pdf;
}

#endif