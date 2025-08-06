// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL
#define MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// Default vertex format
struct RenderableHeader {
    float4 Metadata;
};

struct StaticMeshInstanceHeader {
    // Index of the static mesh which the instance refers to.
    uint StaticMeshIndex;
    uint Padding0;
    uint Padding1;
    uint Padding2;
};

struct VolumePrimitivesInstanceHeader {
    // Index of the volume primitives which the instance refers to.
    uint VolumePrimitivesIndex;
    uint Padding0;
    uint Padding1;
    uint Padding2;
};

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL