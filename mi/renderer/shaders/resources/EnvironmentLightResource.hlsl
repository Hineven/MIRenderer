#ifndef ENVIRONMENT_LIGHT_RESOURCE_HLSL
#define ENVIRONMENT_LIGHT_RESOURCE_HLSL
#include "CommonSamplerResources.hlsl"

TextureCube<float4> EnvironmentMap;

// Direction: light direction (reversed). Same as the direction of radiance proapagation.
float3 EvaluateEnvironmentMap_Raw(float3 Direction, float LOD = 0, float3 Multiplier = 1) {
    return EnvironmentMap.SampleLevel(LinearWrapSampler, Direction, LOD).xyz * Multiplier;
}

#ifdef LIGHT_GRID_HLSL
// Direction: light direction (reversed). Same as the direction of radiance proapagation.
float3 EvaluateEnvironmentMap(float3 Direction) {
    return EnvironmentMap.SampleLevel(LinearWrapSampler, Direction, LightStructure_UB.EnvironmentLightEvaluateLOD).xyz
        * LightStructure_UB.EnvironmentLightMultiplier;
}
#endif

#endif