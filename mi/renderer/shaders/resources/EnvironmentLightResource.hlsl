#ifndef ENVIRONMENT_LIGHT_RESOURCE_HLSL
#define ENVIRONMENT_LIGHT_RESOURCE_HLSL
#include "CommonSamplerResources.hlsl"

TextureCube<float4> EnvironmentMap;

// Direction: light direction (reversed). Same as the direction of radiance proapagation.
float3 EvaluateEnvironmentMap(float3 Direction, float LOD = 0) {
    return EnvironmentMap.SampleLevel(LinearWrapSampler, Direction, LOD).xyz;
}

#endif