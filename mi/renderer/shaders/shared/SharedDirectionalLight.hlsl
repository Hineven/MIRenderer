// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_DIRECTIONAL_LIGHT_HLSL
#define MI_RENDERER_SHADERS_SHARED_DIRECTIONAL_LIGHT_HLSL

#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

struct DirectionalLightUniform {
    // Direction from shading point to light source (normalized)
    float3 ToLightDirection;
    uint Enabled;
    // Directional irradiance / radiance scale (RGB)
    float3 Irradiance;
    uint Padding;
};

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_DIRECTIONAL_LIGHT_HLSL
