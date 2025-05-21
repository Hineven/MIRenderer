#ifndef SHARED_VIEW_HLSL
#define SHARED_VIEW_HLSL

#include "SharedCommon.hlsl"

struct CameraParameters {
    float3 Position;
    float  NearPlane;

    float3 Direction;
    float  FarPlane;

    float3 Up;
    float  FoVY;

    float3 Right;
    // Camera type
    uint  Type;

    uint2   FilmDimensions;
    float2  FilmAspectRatioAndInvAspectRatio;
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

#endif