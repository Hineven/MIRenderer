#ifndef SHARED_DEBUG_HLSL
#define SHARED_DEBUG_HLSL

#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

struct DebugCommonShaderParameters {
    uint2 CursorScreenCoords; // in pixels, (0,0) is top-left corner
    uint  CursorButtonState; // bit 0: left, bit 1: right, bit 2: middle
    uint  Padding0;
};

SHADERONLY(ConstantBuffer<DebugCommonShaderParameters> Debug;)

MI_SHARED_HLSL_END

#endif