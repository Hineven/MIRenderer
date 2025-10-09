#ifndef MATERIAL_HLSL
#define MATERIAL_HLSL

#include "Packing.hlsl"

// Standard material parameters for all shading
struct ShadingMaterial {
    float3 Albedo;
    float3 Emission;
    float3 Normal;
    float Roughness;
    float Metallic;
};

// A tiny representation of a material that can be cached in a few bytes.
// Used for coarse shading on surfaces require reduced shading precision.
struct CachedHitMaterial {
    float3 Albedo;
    // False: volume primitive hit.
    bool bIsSurface;
};

CachedHitMaterial UnpackCachedHitMaterial (uint Packed) {
    CachedHitMaterial M;
    M.Albedo = UnpackUnorm4x8(Packed).rgb;
    uint Flags = (Packed >> 24);
    M.bIsSurface = (Flags & 0x1) != 0;
    return M;
}

uint PackCachedHitMaterial (CachedHitMaterial M) {
    return PackUnorm4x8(float4(M.Albedo, 0)) | (uint(M.bIsSurface ? 1 : 0) << 24);
}

#endif