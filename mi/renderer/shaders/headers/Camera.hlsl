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

float2 ScreenCoordsToNDC2 (CameraParameters C, uint2 ScreenCoords) {
    return UVToNDC2(ScreenCoordsToUV(C, ScreenCoords));
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

float ZDepthToLinearDepth(CameraParameters C, float ZDepth)
{
    float Far = C.FarPlane, Near = C.NearPlane;
    return Far * Near / (Far - ZDepth * (Far - Near));
}

float ReversedZDepthToLinearDepth(CameraParameters C, float ReversedZDepth)
{
    return ZDepthToLinearDepth(C, 1.0f - ReversedZDepth);
}

float3 RecoverWorldPositionNDC2(CameraParameters C, float2 NDC2, float LinearDepth)
{
    return C.Position + NDC2ToCameraDirectionUnnormalized(C, NDC2) * LinearDepth;
}

float3 RecoverWorldPositionPixelCoords(CameraParameters C, uint2 PixelCoords, float LinearDepth)
{
    float2 UV = (PixelCoords + 0.5f.xx) / C.FilmDimensions;
    float2 NDC2 = UVToNDC2(UV);
    return RecoverWorldPositionNDC2(C, NDC2, LinearDepth);
}

float2 GetPixelWorldSize(CameraParameters C, float LinearDepth)
{
    return C.FilmPixelWorldSize * LinearDepth;
}

float3 ReprojectToPreviousUVZFromUVZ(CameraParameters C, float3 UVZ) {
    float3 NDC = float3(UVToNDC2(UVZ.xy), UVZ.z);
    float3 ReprojectedNDC = TransformPoint(C.Reprojection, NDC);
    return float3(NDC2ToUV(ReprojectedNDC.xy), ReprojectedNDC.z);
}

#endif