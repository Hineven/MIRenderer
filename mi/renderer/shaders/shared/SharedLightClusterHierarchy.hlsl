// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_LIGHT_CLUSTER_HIERARCHY_HLSL
#define MI_RENDERER_SHADERS_SHARED_LIGHT_CLUSTER_HIERARCHY_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

struct MeshLightTriangle {
    // The primitive index within its geometry
    uint PrimitiveIndex;
};

struct MeshLightTriangleBakedData {
    // Average emissive power of the triangle. Precomputed on CPU.
    float AvgIntensity;
};

struct MeshLightClusterChild {
    bool bIsLeaf : 1;
    // Mesh light local indices
    // For leaf nodes, this is the index of the MeshLightTriangle. For internal nodes, this is the index of the cluster.
    uint Index : 31;
};

bool IsMeshLightClusterChildValid (MeshLightClusterChild Child) {
    return !(Child.bIsLeaf && Child.Index == 0x7FFFFFFFu); 
}

MeshLightClusterChild MakeInvalidMeshLightClusterChild() {
    MeshLightClusterChild Child;
    Child.bIsLeaf = true;
    Child.Index = 0x7FFFFFFFu;
    return Child;
}

MeshLightClusterChild MakeMeshLightClusterChild(bool bIsLeaf, uint Index) {
    MeshLightClusterChild Child;
    Child.bIsLeaf = bIsLeaf;
    Child.Index = Index;
    return Child;
}

// Indexed by the cluster index.
struct MeshLightClusterHeader {
    uint   MeshLightIndex;
    uint   Hash;
    uint2  Padding;
};

// Indexed by the cluster index.
struct MeshLightClusterNode {
    MeshLightClusterChild L, R; 
};

struct MeshLightInstanceTriangle {
    float3 V0, V1, V2;
    float Intensity;
    uint  MeshLightIndex;
    uint  Hash;
};

// Indexed by the instance cluster index.
struct MeshLightInstanceClusterNode {
    float L_Probability;
    // Tree hierarchy is inherited from the mesh light it derives from. 
};

// Almost the same as MeshLightClusterNode. This is the real stuff injected into the light grid. Indexed by the instance cluster index.
struct MeshLightInstanceClusterHeader {
    // World space AABB
    float3 AABBMin;
    float  WeightedNormalVariance;

    float3 AABBMax;
    // World space weighted normal. 
    uint   WeightedNormal;
    
    uint   MeshLightIndex;
    uint   Hash;
    float  TotalIntensity;
    float  TotalArea;
};

struct MeshLightLevelHeader {
    // Number of clusters in this level.
    uint ClusterCount;
    // (ML local) offset to the first cluster of this level in the cluster buffer. Indexed by MeshLightClusterHeaderBuffer.
    uint ClusterOffset;
};

struct MeshLight {
    // ClusterOffset + MeshLightLocalClusterIndex = index of the cluster in data array
    uint ClusterOffset;
    // TriangleOffset + MeshLightLocalTriangleIndex = index of the ML triangle in data array
    uint TriangleOffset;
    // Index level header meta for each level.
    // Level 0 is the root cluster level.
    uint LevelOffset;
    // The static mesh index the mesh light is built from (24 + 8 bit packed)
    uint StaticMeshIndex;
    uint StaticMeshDescriptionIndex;
    // Number of levels in the hierarchy. 
    uint NumLevels;
    uint NumClusters;
    uint NumTriangles;
};

struct MeshLightInstaceClusterOffset {
    bool bIsTriangle : 1;
    uint Offset : 31;
};

bool IsValid (MeshLightInstaceClusterOffset Offset) {
    return !(Offset.bIsTriangle && Offset.Offset == 0x7FFFFFFFu);
}

// Represent an instance of a mesh light (either persistent in the scene or transient when injected to the light grid)
struct MeshLightInstance {
    uint MeshLightIndex;
    uint RenderableIndex;
    // The offset to the first MLI cluster node of this instance in the MeshLightInstanceClusterNodeBuffer
    // Possibly it is a triangle index if the mesh light has no cluster (NumLevels == 0 in MeshLight).
    MeshLightInstaceClusterOffset MeshLightInstanceClusterOffset;
    uint MeshLightInstanceTriangleOffset;
};

MI_SHARED_HLSL_END
#endif