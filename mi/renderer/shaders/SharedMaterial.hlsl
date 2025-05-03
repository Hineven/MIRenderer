// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL
#define MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// A header of a material. Describe the material in its minimum form.
struct MaterialHeader {
    float3 albedo_ CPPONLY({0.5f});
    float  alpha_ CPPONLY({1.f});
    float3 emissive_ CPPONLY({0.f});
    float  roughness_ CPPONLY({0.5f});
    float3 specular_ CPPONLY({0.f});
    uint  flags_ CPPONLY({});
    // Maps using UV0 (0xffffffffu for no map)
    // index the maps using the renderer readonly texture array.
    uint  albedo_map_ CPPONLY({UINT32_MAX});
    uint  normal_map_ CPPONLY({UINT32_MAX});
    uint  emissive_map_ CPPONLY({UINT32_MAX});
    uint  roughness_map_ CPPONLY({UINT32_MAX});
    // TODO complex material support
};

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL