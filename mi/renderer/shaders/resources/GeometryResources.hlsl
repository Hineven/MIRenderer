#ifndef GEOMETRY_RESOURCES_HLSL
#define GEOMETRY_RESOURCES_HLSL

#include "../shared/SharedLight.hlsl"
#include "../shared/SharedMaterial.hlsl"
#include "../shared/SharedStaticMesh.hlsl"
#include "../shared/SharedRenderable.hlsl"
#include "../shared/SharedVertex.hlsl"

#include "RenderableResources.hlsl"

StructuredBuffer<GeometryHeader> GeometryHeaderBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;
StructuredBuffer<uint> IndexBuffer;

#endif