#ifndef DIRECTIONAL_LIGHT_RESOURCE_HLSL
#define DIRECTIONAL_LIGHT_RESOURCE_HLSL

#include "../shared/SharedDirectionalLight.hlsl"

ConstantBuffer<DirectionalLightUniform> DirectionalLight_UB;

bool DirectionalLightEnabled() {
    return DirectionalLight_UB.Enabled != 0;
}

float3 GetDirectionalLightDirection() {
    return DirectionalLight_UB.ToLightDirection;
}

float3 GetDirectionalLightIrradiance() {
    return DirectionalLight_UB.Irradiance;
}

#endif // DIRECTIONAL_LIGHT_RESOURCE_HLSL
