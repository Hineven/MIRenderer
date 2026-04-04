#ifndef CAMERA_HLSL
#define CAMERA_HLSL

#include "../shared/SharedView.hlsl"
#include "Conversions.hlsl"
#include "Transform.hlsl"

CameraParameters GetActiveCamera() {
    return View.Camera;
}

uint GetViewFrameIndex () {
    return View.FrameIndex;
}

CameraParameters GetPreviousCamera() {
    return View.PreviousCamera;
}

uint GetCameraType (CameraParameters C) {
    return C.Type;
}

// Map (film space) NDC to a direction in world space
float3 NDC2ToCameraDirectionUnnormalized (CameraParameters C, float2 NDC2) {
    if(GetCameraType(C) == CAMERA_TYPE_ORTHOGRAPHIC) {
        return C.Direction;
    }
    NDC2 -= C.Jitter;
    float3 UnnormalizedDirection = C.Right * NDC2.x + C.Up * NDC2.y + C.Direction;
    return UnnormalizedDirection;
}

float3 NDC2ToCameraDirection (CameraParameters C, float2 NDC2) {
    return normalize(NDC2ToCameraDirectionUnnormalized(C, NDC2));
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

float PerspectiveZDepthToLinearDepth(float Near, float Far, float ZDepth)
{
    return Far * Near / (Far - ZDepth * (Far - Near));
}

float ZDepthToLinearDepth(CameraParameters C, float ZDepth)
{
    if(true) {
        float Far = C.FarPlane, Near = C.NearPlane;
        return PerspectiveZDepthToLinearDepth(Near, Far, ZDepth);
    }
}

float LinearDepthToPerspectiveZDepth(float Near, float Far, float LinearDepth)
{
    return (Far * (LinearDepth - Near)) / (LinearDepth * (Far - Near));
}

float LinearDepthToZDepth(CameraParameters C, float LinearDepth)
{
    if(true) {
        float Far = C.FarPlane, Near = C.NearPlane;
        return LinearDepthToPerspectiveZDepth(Near, Far, LinearDepth);
    }
}

float LinearDepthToReversedZDepth(CameraParameters C, float LinearDepth)
{
    return 1.0f - LinearDepthToZDepth(C, LinearDepth);
}

float PerspectiveReversedZDepthToLinearDepth(float Near, float Far, float ReversedZDepth)
{
    return PerspectiveZDepthToLinearDepth(Near, Far, 1.0f - ReversedZDepth);
}

float ReversedZDepthToLinearDepth(CameraParameters C, float ReversedZDepth)
{
    return ZDepthToLinearDepth(C, 1.0f - ReversedZDepth);
}

// Recover world position in the current frame, given NDC2 coordinates and linear depth
float3 RecoverWorldPositionNDC2(CameraParameters C, float2 NDC2, float LinearDepth)
{
    return C.Position + NDC2ToCameraDirectionUnnormalized(C, NDC2) * LinearDepth;
}

// Recover the world position from pixel coordinates in the current frame
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

// Recover a world position from a screen pixel and offset it along the geometry normal
// while limiting the offset to a fraction of the pixel footprint in world space.
float3 RecoverOffsetedWorldPositionFromScreenPixel(
    CameraParameters C,
    uint2 PixelCoords,
    float LinearDepth,
    float3 GeometryNormal,
    float MaxPixelOffsetFraction)
{
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelCoords, LinearDepth);
    float MaxOffsetLength = LinearDepth * 2e-4f;
    float2 PixelSize = GetPixelWorldSize(C, LinearDepth);
    float ProjectionX = abs(dot(C.NormalizedRight, GeometryNormal));
    float ProjectionY = abs(dot(C.NormalizedUp, GeometryNormal));
    float MaxX = MaxPixelOffsetFraction * PixelSize.x / max(ProjectionX, 1e-4f);
    float MaxY = MaxPixelOffsetFraction * PixelSize.y / max(ProjectionY, 1e-4f);
    float OffsetLength = min(MaxOffsetLength, min(MaxX, MaxY));
    return WorldPosition + OffsetLength * GeometryNormal;
}

float3 ReprojectToPreviousUVZFromUVZ(CameraParameters C, float3 UVZ) {
    float3 NDC = float3(UVToNDC2(UVZ.xy), UVZ.z);
    float3 ReprojectedNDC = TransformPoint(C.Reprojection, NDC);
    return float3(NDC2ToUV(ReprojectedNDC.xy), ReprojectedNDC.z);
}

bool IsPointInFrustrum (CameraParameters C, float3 Position, out float3 ViewSpacePosition, bool Ortho = false, float NearClip = 0.f, float Expand = 0.f) {
    ViewSpacePosition = mul(C.WorldToView, float4(Position, 1.0f)).xyz;
    // -z axis is aligned with camera direction
    if(ViewSpacePosition.z >= -NearClip) return false;
    float4 Homogeneous = mul(C.ViewToNDC, float4(ViewSpacePosition, 1.0f));
    if(Ortho) {
        return all(abs(Homogeneous.xy) < 1.0f + Expand) && Homogeneous.z >= 0 && Homogeneous.z <= (1.0f + Expand);
    } else {
        if(Homogeneous.w > 0) {
            float3 Projected = Homogeneous.xyz / Homogeneous.w;
            return all(abs(Projected.xy) < 1.0f + Expand) && Projected.z >= 0.15 && Projected.z <= 1.0f;
        } else {
            return false;
        }
    }
}


#endif
