#ifndef SHARED_VIEW_HLSL
#define SHARED_VIEW_HLSL

#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

struct CameraParameters {
    float3 Position;
    float  NearPlane;

    float3 Direction;
    float  FarPlane;

    float3 Up;
    float  FoVY; // Radians

    float TanFoVY; // tan(FoVY / 2) * 2
    float TanFoVY_2; // tan(FoVY / 2)
    float2 Padding;

    float3 Right;
    // Camera type
    uint  Type;

    float3 NormalizedRight;
    float Padding0;

    float3 NormalizedUp;
    float Padding1;

    uint2  FilmDimensions;
    // Aspect ratio is the viewport width / height
    float2 FilmAspectRatioAndInvAspectRatio;

    uint2 HZBDimensions;
    // Film pixel size in world space (on the LinearDepth == 1 plane).
    // It shold have two identical values if the viewport is not stretched. 
    // (i.e. aspect ratio == FilmDimensions.x / FilmDimensions.y)
    float2 FilmPixelWorldSize;

    float2 InvFilmDimensions;
    float2 UVToHZBScale;

    float2 HZBBaseTexelSize; // 1 / HZBDimensions
    float2 HZBToUVScale;

    // Perspective-View matrix
    float4x4 WorldToNDC;
    // View matrix
    float4x4 WorldToView;
    // Projection matrix
    float4x4 ViewToNDC;

    // Perspective-View matrix with reversed Z (used for rasterization)
    float4x4 WorldToNDC_ReversedZ;
    // Projection matrix with reversed Z
    float4x4 ViewToNDC_ReversedZ;

    float4x4 Reprojection; // current NDC -> previous frame NDC (normal z)
};

struct ViewCommonShaderParameters {
    // Current camera
    CameraParameters Camera;
    // Main camera of the previous frame
    CameraParameters PreviousCamera;

    uint FrameIndex;
};

struct DirectionalLightForShadowMap
{
    float4x4 LightWorldToNDC;
    float3 LightDirWS;
};

#define CAMERA_TYPE_PERSPECTIVE 0u
#define CAMERA_TYPE_ORTHOGRAPHIC 1u

#ifdef __cplusplus
enum class CameraType : uint32_t {
    ePerspective = CAMERA_TYPE_PERSPECTIVE,
    eOrthographic = CAMERA_TYPE_ORTHOGRAPHIC
};
#else
typedef uint CameraType;
#endif


SHADERONLY(ConstantBuffer<ViewCommonShaderParameters> View;)

MI_SHARED_HLSL_END

#endif