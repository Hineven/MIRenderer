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
    float  FoVY;

    float TanFoVY; // tan(FoVY / 2) * 2
    float TanFoVY_2; // tan(FoVY / 2)
    float2 Padding;

    float3 Right;
    // Camera type
    uint  Type;

    uint2   FilmDimensions;
    float2  FilmAspectRatioAndInvAspectRatio;

    // Perspective-View matrix
    float4x4 WorldToNDC;
    // View matrix
    float4x4 WorldToView;
    // Projection matrix
    float4x4 ViewToNDC;
};

struct ViewCommonShaderParameters {
    CameraParameters Camera;
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