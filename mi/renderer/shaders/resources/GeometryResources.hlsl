#ifndef GEOMETRY_RESOURCES_HLSL
#define GEOMETRY_RESOURCES_HLSL

#include "../shared/SharedLight.hlsl"
#include "../shared/SharedMaterial.hlsl"
#include "../shared/SharedStaticMesh.hlsl"
#include "../shared/SharedRenderable.hlsl"
#include "../shared/SharedVertex.hlsl"

#include "RenderableResources.hlsl"

StructuredBuffer<MaterialHeader> MaterialHeaderBuffer;

// StructuredBuffer<uint2> RenderableIndexAndMaterialIndexBuffer;
StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<GeometryHeader> GeometryHeaderBuffer;
StructuredBuffer<uint2> StaticMeshDescriptionBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;
StructuredBuffer<uint> IndexBuffer;

#endif