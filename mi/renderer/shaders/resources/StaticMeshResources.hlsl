#ifndef STATIC_MESH_RESOURCES_HLSL
#define STATIC_MESH_RESOURCES_HLSL

#include "../shared/SharedStaticMesh.hlsl"
#include "GeometryResources.hlsl"

StructuredBuffer<uint2>            StaticMeshDescriptionBuffer;
StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;


#endif