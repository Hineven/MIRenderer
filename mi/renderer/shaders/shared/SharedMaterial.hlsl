// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL
#define MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

#define MATERIAL_FLAG_POINT_SAMPLED (0x1u)
#define MATERIAL_FLAG_FORWARD       (0x2u)
#define MATERIAL_FLAG_DOUBLE_SIDED  (0x4u)

// A header of a material. Describe the material in its minimum form.
struct MaterialHeader {
    float3 Albedo CPPONLY({0.5f});
    uint  Flags CPPONLY({});
    float3 Emissive CPPONLY({0.f});
    float  Roughness CPPONLY({0.5f});
    float Metallic CPPONLY({0.5f});
    float3 SpecularTint CPPONLY({1.f});
    // Maps using UV0 (0xffffffffu for no map)
    // index the maps using the renderer readonly texture array.
    uint  AlbedoMap CPPONLY({UINT32_MAX});
    uint  NormalMap CPPONLY({UINT32_MAX});
    uint  EmissiveMap CPPONLY({UINT32_MAX});
    uint  MetallicRoughnessMap CPPONLY({UINT32_MAX});
    // TODO complex material support
};

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL