// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL
#define MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// Default vertex format
struct DefaultStaticMeshVertex {
    glm::vec3 position SEMANTICS(position);
    glm::vec3 normal SEMANTICS(normal);
    glm::vec2 uv SEMANTICS(uv);
};

MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL