#ifndef INTERSECTION_HLSL
#define INTERSECTION_HLSL

#include "../shared/SharedVertex.hlsl"

struct IntersectionMaterial {
    float3 Albedo;
    float Opacity;
    float3 Normal;
    float3 Emission;
    float2 MetallicRoughness;
};

#endif