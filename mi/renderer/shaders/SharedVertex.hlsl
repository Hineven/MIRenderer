// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL
#define MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// Default vertex format
struct DefaultStaticMeshVertex {
    float3 position SEMANTICS(position);
    float3 normal SEMANTICS(normal);
    float2 uv SEMANTICS(uv);
};

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL