// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_LIGHT_HLSL
#define MI_RENDERER_SHADERS_SHARED_LIGHT_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

#define LIGHT_FLAG_DIRTY (0x1u << 31)
#define LIGHT_FLAG_HASH_MASK (0x7f000000u)
struct RawLight {
    // LightData
    // Data.x: Renderable index (uint)
    // Data.y: Static mesh Geopmetry-Material pari descriptor index (uint)
    // Data.z: Primitive index (uint)
    // Data.w: Flags (uint)
    // Low end | dirty(1bit) | Hash(31bit) | High end
    uint4 Data0;
};

struct AreaLight {
    uint RenderableIndex;
    uint StaticMeshDescriptionIndex;
    uint PrimitiveIndex;
    uint Flags;
};

#define PRECOMPUTED_LIGHT_TYPE_TRIANGLE 0
#define PRECOMPUTED_LIGHT_TYPE_AREA 1
#define PRECOMPUTED_LIGHT_TYPE_DIRECTIONAL 2

// Precompute lights, making it easier to estiamte their contributions
struct PrecomputedLight {
    // Triangle light: Triangle vertices
    // Area light: V0 and V1.x: Packed spheciral octahedon UV (non-area-preserving mapping) boundaries
    // Directional light: V0 is the direction (normalized).
    float3 V0, V1, V2;
    // Triangle normal. Makes sense only for triangle lights
    float3 Normal;
    float Intensity;
    bool bInvalid;
    // One of the above types
    uint Type;
};

struct PackedPrecomputedLight {
    // Triangle vertices. For non-triangle lights, V2 packs the type using its lower bits
    float3 V0, V1, V2;
    // Triangle normal. For non-triangle lights, this is INVALID_UINT
    uint Normal;       
    float Intensity;
};

MI_SHARED_HLSL_END
#endif