// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL
#define MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

// Default vertex format
struct DefaultStaticMeshVertex {
    float3 Position SEMANTICS(position);
    float3 Normal SEMANTICS(normal);
    float2 UV SEMANTICS(uv);
};

#ifdef MI_SHADER
DefaultStaticMeshVertex InterpolateVertex(DefaultStaticMeshVertex C, DefaultStaticMeshVertex A, DefaultStaticMeshVertex B, float2 Barycentric) {
    DefaultStaticMeshVertex Result;
    float Z = (1 - Barycentric.x - Barycentric.y);
    Result.Position = A.Position * Barycentric.x + B.Position * Barycentric.y + C.Position * Z;
    Result.Normal = normalize(A.Normal * Barycentric.x + B.Normal * Barycentric.y + C.Normal * Z);
    Result.UV = A.UV * Barycentric.x + B.UV * Barycentric.y + C.UV * Z;
    return Result;
}
#endif


MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL