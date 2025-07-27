#ifndef GEOMETRY_BUFFERS_HLSL
#define GEOMETRY_BUFFERS_HLSL

#include "Camera.hlsl"

float ReversedZDepthToLinearDepth(CameraParameters C, float ReversedZDepth)
{
    float Far = C.FarPlane, Near = C.NearPlane;
    float ZDepth = 1.f - ReversedZDepth;
    return Far * Near / (Far - ZDepth * (Far - Near));
}

float3 RecoverWorldPosition(CameraParameters C, uint2 PixelCoords, float ReversedZDepth)
{
    float2 UV = (PixelCoords + 0.5f.xx) / C.FilmDimensions;
    float2 NDC2 = UVToNDC2(UV);
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    return C.Position + NDC2ToCameraDirectionUnnormalized(C, NDC2) * LinearDepth;
}

#endif