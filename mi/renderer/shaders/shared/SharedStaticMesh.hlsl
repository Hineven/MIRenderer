// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_STATIC_MESH_HLSL
#define MI_RENDERER_SHADERS_SHARED_STATIC_MESH_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

struct StaticMeshHeader {
    uint32_t DescriptionOffset; // Offset in the static mesh description heap, in num-entries
    uint32_t NumGeometries; // Number of material-geometry pairs in the static mesh description
};

struct GeometryHeader {
    uint32_t VertexOffset; // Offset in the vertex buffer heap, in num elements
    uint32_t IndexOffset; // Offset in the index buffer heap, in num elements
    uint32_t VertexCount; // Number of vertices in the geometry
    uint32_t IndexCount; // Number of indices in the geometry
};

MI_SHARED_HLSL_END

#endif