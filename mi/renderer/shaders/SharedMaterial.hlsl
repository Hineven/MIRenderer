// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL
#define MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// A header of a material. Describe the material in its minimum form.
struct MaterialHeader {
    float3 albedo_ CPPONLY({0.5f});
    float alpha_ CPPONLY({1.f});
    glm::vec3 emissive_ CPPONLY({0.f});
    float roughness_ CPPONLY({0.5f});
    glm::vec3 specular_ CPPONLY({0.f});
    uint32_t flags_ CPPONLY({});
    // Maps using UV0 (0xffffffffu for no map)
    // index the maps using the renderer readonly texture array.
    uint32_t albedo_map_ CPPONLY({UINT32_MAX});
    uint32_t normal_map_ CPPONLY({UINT32_MAX});
    uint32_t emissive_map_ CPPONLY({UINT32_MAX});
    uint32_t roughness_map_ CPPONLY({UINT32_MAX});
    // TODO complex material support
};

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_MATERIAL_HLSL