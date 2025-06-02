// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL
#define MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// Default vertex format
struct RenderableHeader {
    float4 Metadata;
};

struct StaticMeshRenderableHeader {
    uint NumGeometries;
    uint Padding0;
    uint Padding1;
    uint Padding2;
};

struct VolumePrimitivesRenderableHeader {
    uint NumPrimitives;
    uint Padding0;
    uint Padding1;
    uint Padding2;
};

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_RENDERABLE_HLSL