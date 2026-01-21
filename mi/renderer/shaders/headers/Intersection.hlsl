#ifndef INTERSECTION_HLSL
#define INTERSECTION_HLSL

#include "Material.hlsl"

struct IntersectionMaterial {
    float3 LocalPosition; // Renderable local space position
    float3 WorldPosition; // World space position
    float3 Albedo;
    float Opacity;
    float3 Normal;
    float3 Emission;
    float2 MetallicRoughness;
    bool bDoubleSided;
};

ShadingMaterial GetShadingMaterial(IntersectionMaterial M) {
    ShadingMaterial Result;
    Result.Albedo = M.Albedo;
    Result.Emission = M.Emission;
    Result.Normal = M.Normal;
    Result.Roughness = M.MetallicRoughness.y;
    Result.Metallic = M.MetallicRoughness.x;
    Result.bDoubleSided = M.bDoubleSided;
    return Result;
}

#endif