#ifndef SHARED_VOLUME_GRID_HLSL
#define SHARED_VOLUME_GRID_HLSL

#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

/**
 * 体积网格头数据
 */
struct VolumeGridHeader {
    uint TextureBindlessIndex; // 对应 3D 纹理的 Bindless 句柄
    uint3 LocalMin;            // 局部空间包围盒 Min (用于 UV 归一化计算)

    uint3 LocalMax;            // 局部空间包围盒 Max
    uint _Padding0;
};

MI_SHARED_HLSL_END

#endif