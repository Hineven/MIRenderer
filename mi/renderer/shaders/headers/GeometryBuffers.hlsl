#ifndef GEOMETRY_BUFFERS_HLSL
#define GEOMETRY_BUFFERS_HLSL

#include "Camera.hlsl"
#include "Transform.hlsl"

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

// This bit on the flags texture indicates that the pixel is invalid for SSRT to 
// step through. For example, volumetric primitives, or some other geometry that 
// doesn't have proper depth information.
#define FLAG_BITS_TEXTURE_INVALID_FOR_SSRT 0x1

#endif