#ifndef SHARED_VIEW_HLSL
#define SHARED_VIEW_HLSL

#include "SharedCommon.hlsl"

BEGIN_SHADER_PARAMETERS(CameraParameters)
    SHADER_PARAMETER(float3, Position)
    SHADER_PARAMETER(float,  NearPlane)

    SHADER_PARAMETER(float3, Direction)
    SHADER_PARAMETER(float,  FarPlane)

    SHADER_PARAMETER(float3, Up)
    SHADER_PARAMETER(float,  FoVY)

    SHADER_PARAMETER(float3, Right)
    // Camera type
    SHADER_PARAMETER(uint,  Type)

    SHADER_PARAMETER(uint2,   FilmDimensions)
    SHADER_PARAMETER(float2,  FilmAspectRatioAndInvAspectRatio)
END_SHADER_PARAMETERS()

BEGIN_SHADER_PARAMETERS(ViewCommonShaderParameters)
    SHADER_PARAMETER_STRUCT_NESTED(CameraParameters, Camera)
END_SHADER_PARAMETERS()

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