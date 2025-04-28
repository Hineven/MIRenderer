// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_COMMON_HLSL
#define MI_RENDERER_SHADERS_SHARED_COMMON_HLSL

#ifdef __cplusplus
#include <glm/glm.hpp>
#include "core/common.h"
MI_NAMESPACE_BEGIN

typedef glm::vec2 float2;
typedef glm::vec3 float3;
typedef glm::vec4 float4;
typedef glm::mat2 float2x2;
typedef glm::mat3 float3x3;
typedef glm::mat4 float4x4;
// Orders of the matrix are different in glm and HLSL. (col major vs row major)
// (However the memory order of the matrix declared in HLSL constant blocks are defaulted to col major.
// So we do not need to explicit transit that when uploading uniform buffers.)
typedef glm::mat3x4 float4x3;

typedef uint32_t uint;
typedef glm::uvec2 uint2;
typedef glm::uvec3 uint3;
typedef glm::uvec4 uint4;

typedef glm::ivec2 int2;
typedef glm::ivec3 int3;
typedef glm::ivec4 int4;

#define MI_SHARED_HLSL_BEGIN MI_NAMESPACE_BEGIN
#define MI_SHARED_HLSL_END MI_NAMESPACE_END

#define SEMANTICS(name)
#define CPPONLY(name) name

#else
#define MI_SHARED_HLSL_BEGIN
#define MI_SHARED_HLSL_END
#define SEMANTICS(name) : name
#define CPPONLY(name)
#endif

#ifdef __cplusplus
MI_NAMESPACE_END
#endif

#endif // MI_RENDERER_SHADERS_SHARED_COMMON_HLSL