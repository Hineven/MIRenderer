#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL

struct ShadingMaterial {
    float3 Albedo;
    float3 Emission;
    float3 Normal;
    float Roughness;
    float Metallic;
};

#endif