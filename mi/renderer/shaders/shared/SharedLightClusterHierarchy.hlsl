// This file is shared between shaders and C++ code.
#ifndef MI_RENDERER_SHADERS_SHARED_LIGHT_CLUSTER_HIERARCHY_HLSL
#define MI_RENDERER_SHADERS_SHARED_LIGHT_CLUSTER_HIERARCHY_HLSL
#include "SharedCommon.hlsl"

MI_SHARED_HLSL_BEGIN

struct MeshLightTriangle {
    // The primitive index within its geometry
    uint PrimitiveIndex;
};

struct MeshLightTriangleHash {
    uint Hash;
};

struct MeshLightTriangleBakedData {
    // Average emissive power of the triangle. Precomputed on CPU.
    float AvgIntensity;
};

// A child node in the mesh light cluster hierarchy. It can be either an inner node (cluster) or a leaf node (triangle).
// This is a offset to the first cluster node / triangle of the MeshLight structure. Not an absolute offset to the buffer.
struct MeshLightClusterChild {
    uint Packed;
    // Whether this child is a leaf node (triangle) or an inner node (cluster). If it's a leaf node, the index is the triangle index. If it's an inner node, the index is the cluster index.
    bool bIsLeaf () {
        return (Packed & 0x80000000u) != 0;
    }
    // Mesh light local indices
    // For leaf nodes, this is the index of the MeshLightTriangle. For internal nodes, this is the index of the cluster.
    uint Index () {
        return Packed & 0x7FFFFFFFu;
    }
};

bool IsMeshLightClusterChildValid (MeshLightClusterChild Child) {
    return !(Child.bIsLeaf && Child.Index == 0x7FFFFFFFu); 
}

MeshLightClusterChild MakeInvalidMeshLightClusterChild() {
    MeshLightClusterChild Child;
    Child.Packed = 0xFFFFFFFFu;
    return Child;
}

MeshLightClusterChild MakeMeshLightClusterChild(bool bIsLeaf, uint Index) {
    MeshLightClusterChild Child;
    Child.Packed = (bIsLeaf ? 0x80000000u : 0) | (Index & 0x7FFFFFFFu);
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
    uint  MeshLightInstanceIndex;
    uint  Hash;
};

// Indexed by the instance cluster index.
struct MeshLightInstanceClusterNode {
    float L_Probability;
    // Tree hierarchy is inherited from the mesh light it derives from. 
};

// Almost the same as MeshLightClusterNode (but this one is in world space with applied transforms).
// This is the real stuff injected into the light grid. Indexed by the instance cluster index.
struct MeshLightInstanceClusterHeader {
    // World space AABB
    float3 AABBMin;
    float  WeightedNormalVariance;

    float3 AABBMax;
    // World space weighted normal. 
    uint   WeightedNormal;
    
    uint   MeshLightInstanceIndex;
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

    bool bIsTriangle () {
        return NumLevels == 0;
    }
};

// An offset into the MLI cluster / triangle buffer. Usually the offset is an absolute offset relative to the buffer start.
// Can be a reference to a specific cluster / triangle of the MLI structure.
struct MeshLightInstanceElementOffset {
    uint Packed;

    bool IsValid () {
        return Packed != 0xFFFFFFFFu;
    }
    bool bIsTriangle() {
        return (Packed & 0x80000000u) != 0;
    }
    uint Offset() {
        return Packed & 0x7FFFFFFFu;
    }
};

bool IsValid(MeshLightInstanceElementOffset Element) {
    return Element.IsValid();
}

MeshLightInstanceElementOffset MakeMeshLightInstanceElementOffset(bool bIsTriangle, uint Offset) {
    MeshLightInstanceElementOffset Element;
    Element.Packed = (bIsTriangle ? 0x80000000u : 0) | (Offset & 0x7FFFFFFFu);
    return Element;
}

// Represent an instance of a mesh light (either persistent in the scene or transient when injected to the light grid)
struct MeshLightInstance {
    uint MeshLightIndex;
    uint RenderableIndex;
    // The offset to the first MLI cluster node of this instance in the MeshLightInstanceClusterNodeBuffer
    // Possibly it is a triangle index if the mesh light has no cluster (NumLevels == 0 in MeshLight).
    MeshLightInstanceElementOffset MeshLightInstanceClusterOffset;
    uint MeshLightInstanceTriangleOffset;
};

MI_SHARED_HLSL_END
#endif