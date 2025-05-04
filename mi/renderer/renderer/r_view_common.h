/*
 * Created: 2025/5/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef R_VIEW_COMMON_H
#define R_VIEW_COMMON_H

#include "r_internal_common.h"

MI_NAMESPACE_BEGIN

BEGIN_SHADER_PARAMETERS(ViewCommonShaderParameters)
    SHADER_PARAMETER(float3, CameraPosition)
    SHADER_PARAMETER(float,  CameraNearPlane)

    SHADER_PARAMETER(float3, CameraDirection)
    SHADER_PARAMETER(float,  CameraFarPlane)

    SHADER_PARAMETER(float3, CameraUp)
    SHADER_PARAMETER(float,  CameraFoVY)

    SHADER_PARAMETER(uint2,  FilmDimensions)
    SHADER_PARAMETER(uint2,  FilmAspectRatio)
END_SHADER_PARAMETERS()

MI_NAMESPACE_END

#endif //R_VIEW_COMMON_H
