// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_LIGHT_HLSL
#define MI_RENDERER_SHADERS_SHARED_LIGHT_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

#define LIGHT_FLAG_DIRTY (0x1u << 31)
#define LIGHT_FLAG_HASH_MASK (0x7f000000u)
struct Light {
    // LightData
    // Data.x: Renderable index (uint)
    // Data.y: Static mesh Geopmetry-Material pari descriptor index (uint)
    // Data.z: Primitive index (uint)
    // Data.w: Flags (uint)
    // Low end | dirty(1bit) | Hash(7bit) | Unused(24bit) | High end
    uint4 Data0;
};

MI_SHARED_HLSL_END
#endif