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
    bool bDoubleSided;
};

#define CACHED_HIT_MATERIAL_HIT_TYPE_SURFACE 0
#define CACHED_HIT_MATERIAL_HIT_TYPE_VOLUME 1
#define CACHED_HIT_MATERIAL_HIT_TYPE_GAUSSIAN 2
#define CACHED_HIT_MATERIAL_HIT_TYPE_INVALID 0xF

uint GetCachedHitMaterialHitFlags (uint2 CM) {
    return CM.x >> 24;
}

// 4 bits for hit type
uint GetCachedHitMaterialHitType (uint2 CM) {
    return GetCachedHitMaterialHitFlags(CM) & 0xF;
}

// A tiny representation of a material that can be cached in a few bytes.
// Used for coarse shading on shading points require reduced shading precision.
struct CachedHitMaterial {
    uint   HitType;
    float3 Albedo; // Valid for all hit types
    float3 Normal; // Valid for surface hits
    bool   bValid; // Whether the hit is valid
    bool IsSurface () {
        return HitType == CACHED_HIT_MATERIAL_HIT_TYPE_SURFACE;
    }
    bool IsVolume () {
        return HitType == CACHED_HIT_MATERIAL_HIT_TYPE_VOLUME;
    }
};

CachedHitMaterial UnpackCachedHitMaterial (uint2 Packed) {
    CachedHitMaterial M;
    M.Albedo = UnpackUnorm4x8(Packed.x).rgb;
    uint Flags = (Packed.x >> 24);
    M.HitType = Flags & 0xFF;
    M.bValid = (Flags != 0xFF);
    M.Normal = UnpackNormal(Packed.y);
    return M;
}

uint2 PackCachedHitMaterial (CachedHitMaterial M) {
    uint HighByte = M.bValid ? 0 : 0xFF;
    HighByte |= (M.HitType & 0xFF);
    uint X = PackUnorm4x8(float4(M.Albedo, 0)) | (HighByte << 24);
    uint Y = PackNormal(M.Normal);
    return uint2(X, Y);
}

uint2 MakePackedInvalidCachedHitMaterial () {
    return uint2(0xFFFFFFFF, 0xFFFFFFFF);
}

CachedHitMaterial MakeCachedHitMaterial(float3 Albedo, uint HitType, float3 Normal = 0) {
    CachedHitMaterial M;
    M.Albedo = Albedo;
    M.HitType = HitType;
    M.bValid = true;
    M.Normal = Normal;
    return M;
}

#endif