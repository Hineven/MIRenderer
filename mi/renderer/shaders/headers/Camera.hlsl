#ifndef CAMERA_HLSL
#define CAMERA_HLSL

#include "../shared/SharedView.hlsl"
#include "Conversions.hlsl"
#include "Transform.hlsl"

CameraParameters GetActiveCamera() {
    return View.Camera;
}

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

float2 ScreenCoordsToUV (CameraParameters C, uint2 ScreenCoords) {
    return (ScreenCoords + 0.5f.xx) * C.InvFilmDimensions;
}

float2 PixelPositionToUV (CameraParameters C, float2 PixelPosition) {
    return PixelPosition * C.InvFilmDimensions;
}

float2 UVToPixelPosition (CameraParameters C, float2 UV) {
    return UV * C.FilmDimensions;
}

float2 ScreenPositionToUV (CameraParameters C, float2 ScreenPosition) {
    return ScreenPosition * C.InvFilmDimensions;
}

float2 ScreenPositionToNDC2 (CameraParameters C, float2 ScreenPosition) {
    return UVToNDC2(ScreenPositionToUV(C, ScreenPosition));
}

float2 NDC2ToScreenPosition (CameraParameters C, float2 NDC2) {
    return NDC2ToUV(NDC2) * C.FilmDimensions;
}

float3 ReprojectToPreviousNDCFromNDC (CameraParameters C, float3 NDC) {
    return TransformPoint(C.Reprojection, NDC);
}

#endif