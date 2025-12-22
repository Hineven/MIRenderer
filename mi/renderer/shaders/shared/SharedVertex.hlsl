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
DefaultStaticMeshVertex InterpolateVertex(DefaultStaticMeshVertex A, DefaultStaticMeshVertex B, DefaultStaticMeshVertex C, float2 Barycentric) {
    DefaultStaticMeshVertex Result;
    float a = (1 - Barycentric.x - Barycentric.y);
    float b = Barycentric.x;
    float c = Barycentric.y;
    Result.Position = a * A.Position + b * B.Position + c * C.Position;
    Result.Normal = a * A.Normal + b * B.Normal + c * C.Normal;
    Result.UV = a * A.UV + b * B.UV + c * C.UV;
    return Result;
}
#endif


MI_SHARED_HLSL_END

#endif // MI_RENDERER_SHADERS_SHARED_VERTEX_HLSL