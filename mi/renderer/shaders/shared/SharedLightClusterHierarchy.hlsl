// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_LIGHT_CLUSTER_HIERARCHY_HLSL
#define MI_RENDERER_SHADERS_SHARED_LIGHT_CLUSTER_HIERARCHY_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

struct MeshLightTriangle {
    // The primitive index within its geometry
    uint PrimitiveIndex;
};

struct MeshLightClusterChild {
    bool bIsLeaf : 1;
    // For leaf nodes, this is the index of the MeshLightTriangle. For internal nodes, this is the index of the cluster.
    uint Index : 31;
};

// Indexed by the cluster index.
struct MeshLightClusterHeader {
    float3 LocalAABBMin {};
    float  WeightedNormalVariance {};
    float3 LocalAABBMax {};
    float  TotalIntensity {};
    float3 WeightedNormal;
    uint   Hash;
};

// Indexed by the cluster index.
struct MeshLightClusterNode {
    // Simple intensity based splitting heuristic. Only taking a small amount of storage.
    float L_Weight, R_Weight;
    MeshLightClusterChild L, R;
};

MI_SHARED_HLSL_END
#endif