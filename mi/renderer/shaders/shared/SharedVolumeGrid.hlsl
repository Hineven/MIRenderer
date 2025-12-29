#ifndef SHARED_VOLUME_GRID_HLSL
#define SHARED_VOLUME_GRID_HLSL

#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

/**
 * Volume Grid Header Structure
 */
struct VolumeGridHeader {
    uint TextureBindlessIndex;
    float3 LocalMin;

    float3 LocalMax;
    uint _Padding0;
};

MI_SHARED_HLSL_END

#endif