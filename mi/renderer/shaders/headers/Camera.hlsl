#ifndef CAMERA_HLSL
#define CAMERA_HLSL

#include "../shared/SharedView.hlsl"
#include "Conversions.hlsl"

uint GetCameraType (CameraParameters C) {
    return C.Type;
}

// Map (film space) NDC to a direction in world space
float3 NDC2ToCameraDirectionUnnormalized (CameraParameters C, float2 NDC2) {
    if(GetCameraType(C) == CAMERA_TYPE_ORTHOGRAPHIC) {
        return C.Direction;
    }
    float3 UnnormalizedDirection = C.Right * NDC2.x + C.Up * NDC2.y + C.Direction;
    return UnnormalizedDirection;
}

float3 NDC2ToCameraDirection (CameraParameters Camera, float2 NDC2) {
    return normalize(NDC2ToCameraDirectionUnnormalized(Camera, NDC2));
}

float3 NDC2ToCameraOrigin (CameraParameters C, float2 NDC2) {
    if(GetCameraType(C) == CAMERA_TYPE_ORTHOGRAPHIC) {
        return C.Position + C.Right * NDC2.x + C.Up * NDC2.y;
    }
    return C.Position;
}

#endif