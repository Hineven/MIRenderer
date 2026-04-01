// The shader performs light precomputation & injection for all important scene lights

#include "headers/Random.hlsl"
#include "resources/StaticMeshResources.hlsl"
#include "resources/LightClusterHierarchyResources.hlsl"
#include "resources/LightGridSampling.hlsl"


#ifndef THREAD_GROUP_SIZE
#define THREAD_GROUP_SIZE 128
#endif
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void LightPrecomputation_ClearCounters (uint DispatchID : SV_DispatchThreadID) {
    LightGrid_RWActiveGridAllocator[0] = 0;
    LightGrid_RWListAllocator[0]   = 0;
    uint Index = DispatchID;
    if (Index >= LightStructure_UB.LightGridNumGrids) {
        return;
    }
    LightGrid_RWGridLightListLengthBuffer[Index] = 0;
}

// Gather all active light grids for light injection
#ifndef REPEAT_COUNT
#define REPEAT_COUNT 8
#endif

#ifndef WAVE_SIZE
#define WAVE_SIZE 32
#endif

[numthreads(WAVE_SIZE, 1, 1)]
void LightPrecomputation_GatherActiveGrids(uint GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint StartID = GroupID * WAVE_SIZE * REPEAT_COUNT;
    bool bIsLastLane = WaveGetLaneIndex() == WAVE_SIZE - 1;
    uint TotalPrefixSumInclusive = 0;
    uint LocalOffsets[REPEAT_COUNT];
    for(int i = 0; i < REPEAT_COUNT; i++) {
        uint GridIndex = StartID + i * WAVE_SIZE + LocalID;
        uint Value = 0;
        if(GridIndex < LightStructure_UB.LightGridNumGrids) {
            Value = LightGrid_RWActiveGridFlagBuffer[GridIndex];
            // Keep 3 frames of history for each light grid to avoid jittering of light injection. 
            LightGrid_RWActiveGridFlagBuffer[GridIndex] = (Value << 1) & 0xfu;
        }
        uint Value01 = Value > 0 ? 1 : 0; // Whether this grid is active in the current frame
        uint OuterPrefixSum = WaveReadLaneAt(TotalPrefixSumInclusive, WAVE_SIZE - 1);
        uint WavePrefixSumInclusive = WavePrefixSum(Value01) + Value01;
        TotalPrefixSumInclusive = OuterPrefixSum + WavePrefixSumInclusive;
        LocalOffsets[i] = Value01 | ((TotalPrefixSumInclusive - Value01) << 1); // Exclusive prefix sum
    }
    uint GlobalOffset = 0;
    if(bIsLastLane) {
        uint TotalActiveGrids = TotalPrefixSumInclusive;
        InterlockedAdd(LightGrid_RWActiveGridAllocator[0], TotalActiveGrids, GlobalOffset);
    }
    GlobalOffset = WaveReadLaneAt(GlobalOffset, WAVE_SIZE - 1);
    for(int i = 0; i < REPEAT_COUNT; i++) {
        uint GridIndex = StartID + i * WAVE_SIZE + LocalID;
        if(GridIndex < LightStructure_UB.LightGridNumGrids && (LocalOffsets[i] & 1) == 1) {
            uint WriteIndex = GlobalOffset + (LocalOffsets[i] >> 1);
            if(WriteIndex < LightStructure_UB.MaxNumActiveLightGrids) {
                LightGrid_RWActiveGridIndicesBuffer[WriteIndex] = GridIndex;
            }
        }
    }
}

// Precompute each instanced triangle in MLIs
void LightPrecomputation_Triangle (uint VertexID : SV_VertexID, uint InstanceID : SV_InstanceID) {
    uint MeshLightLocalTriangleIndex = VertexID;
    uint MeshLightInstanceIndex = LightGrid_ActiveMeshLightInstanceIndexBuffer[InstanceID];
    
    MeshLightInstance MLI = LCH_MeshLightInstanceBuffer[MeshLightInstanceIndex];
    MeshLight ML = LCH_MeshLightBuffer[MLI.MeshLightIndex];
    float3x4 RenderableToWorld = RenderableTransformBuffer[MLI.RenderableIndex];

    uint MeshLightInstanceTriangleIndex = MLI.MeshLightInstanceTriangleOffset + MeshLightLocalTriangleIndex;
    uint MeshLightTriangleIndex = ML.TriangleOffset + MeshLightLocalTriangleIndex;

    MeshLightTriangle MLTriangle = LCH_MeshLightTriangleBuffer[MeshLightTriangleIndex];
    MeshLightTriangleBakedData BakedData = LCH_MeshLightTriangleBakedDataBuffer[MeshLightTriangleIndex];

    // Read ML triangle geometry
    uint GeometryLocalPrimitiveIndex = MLTriangle.PrimitiveIndex;
    StaticMeshHeader MeshHeader = StaticMeshHeaderBuffer[ML.StaticMeshIndex];
    uint GeometryIndex = StaticMeshDescriptionBuffer[MeshHeader.DescriptionOffset + ML.StaticMeshDescriptionIndex].x;
    GeometryHeader MeshGeometry = GeometryHeaderBuffer[GeometryIndex];
    uint GeometryTriangleIndexOffset = MeshGeometry.IndexOffset + GeometryLocalPrimitiveIndex * 3;
    uint VertexOffset = MeshGeometry.VertexOffset;
    uint i0 = VertexOffset + IndexBuffer[GeometryTriangleIndexOffset];
    uint i1 = VertexOffset + IndexBuffer[GeometryTriangleIndexOffset + 1];
    uint i2 = VertexOffset + IndexBuffer[GeometryTriangleIndexOffset + 2];
    DefaultStaticMeshVertex V0 = VertexBuffer[i0];
    DefaultStaticMeshVertex V1 = VertexBuffer[i1];
    DefaultStaticMeshVertex V2 = VertexBuffer[i2];
    // Transform MLTriangle to world space.
    MeshLightInstanceTriangle MLITriangle = (MeshLightInstanceTriangle)0;
    MLITriangle.V0 = TransformPoint(RenderableToWorld, V0.Position);
    MLITriangle.V1 = TransformPoint(RenderableToWorld, V1.Position);
    MLITriangle.V2 = TransformPoint(RenderableToWorld, V2.Position);
    float Area = length(cross(MLITriangle.V1 - MLITriangle.V0, MLITriangle.V2 - MLITriangle.V0) * 0.5f);
    MLITriangle.Intensity = BakedData.AvgIntensity * Area;
    MLITriangle.MeshLightInstanceIndex = MeshLightInstanceIndex;
    MLITriangle.Hash = LCH_MeshLightTriangleHashBuffer[MeshLightTriangleIndex].Hash;
    // Write to output buffer.
    LCH_RWMeshLightInstanceTriangleBuffer[MeshLightInstanceTriangleIndex] = MLITriangle;
}

// Each thread process 4 - 2 - 1 (PROCESSING_LEVELS_PER_DISPATCH) nodes in the hierarchy. The total number of dispatches is ceil(MaxDepth / PROCESSING_LEVELS_PER_DISPATCH).
#ifndef PROCESSING_LEVELS_PER_DISPATCH
#define PROCESSING_LEVELS_PER_DISPATCH 3
#endif

struct LightPrecomputationLevelUB {
    uint LevelIndex;
    uint3 Padding;
};
ConstantBuffer<LightPrecomputationLevelUB> LightPrecomputation_LevelUB;

// TODO modify this function to use the data from MeshLightInstanceTriangleBuffer
void AccumulateMeshLightInstanceClusterNodeDataFromTriangleChild (
    MeshLightClusterChild TriangleChild,
    MeshLight ML,
    MeshLightInstance MLI,
    GeometryHeader MeshGeometry,
    float3x4 RenderableToWorld,
    inout MeshLightInstanceClusterHeader MLIClusterHeader,
    inout float3 WeightedNormal, out float IntensityWeight
) {
    uint MeshLightLocalTriangleIndex = TriangleChild.Index();
    uint MLITriangleIndex = MLI.MeshLightInstanceTriangleOffset + MeshLightLocalTriangleIndex;
    // Extract triangle data
    MeshLightInstanceTriangle MLITriangle = LCH_RWMeshLightInstanceTriangleBuffer[MLITriangleIndex];
    uint MeshLightTriangleIndex = ML.TriangleOffset + MeshLightLocalTriangleIndex;
    // Accumulate to cluster header
    MLIClusterHeader.AABBMin = min(MLIClusterHeader.AABBMin, min(MLITriangle.V0, min(MLITriangle.V1, MLITriangle.V2)));
    MLIClusterHeader.AABBMax = max(MLIClusterHeader.AABBMax, max(MLITriangle.V0, max(MLITriangle.V1, MLITriangle.V2)));
    float3 Normal = normalize(cross(MLITriangle.V1 - MLITriangle.V0, MLITriangle.V2 - MLITriangle.V0));
    MeshLightTriangleBakedData BakedData = LCH_MeshLightTriangleBakedDataBuffer[MeshLightTriangleIndex];
    float  Area = length(cross(MLITriangle.V1 - MLITriangle.V0, MLITriangle.V2 - MLITriangle.V0)) * 0.5f;
    float  Intensity = BakedData.AvgIntensity * Area; 
    float3 WeightedTriangleNormal = Normal * Intensity;
    WeightedNormal += WeightedTriangleNormal;
    MLIClusterHeader.TotalIntensity += Intensity;
    MLIClusterHeader.TotalArea += Area;
    // Store WeightedNormal ^ 2 first. A separate finalization will be performed later.
    MLIClusterHeader.WeightedNormalVariance += dot(WeightedTriangleNormal, WeightedTriangleNormal);
    IntensityWeight = Intensity;
}

void AccumulateMeshLightInstanceClusterNodeDataFromClusterChild (
    MeshLightClusterChild ClusterChild,
    MeshLight ML,
    MeshLightInstanceClusterHeader MLIChildClusterHeader,
    float3x4 RenderableToWorld,
    inout MeshLightInstanceClusterHeader MLIClusterHeader,
    inout float3 WeightedNormal, out float IntensityWeight
) {
    // Accumulate to cluster header
    MLIClusterHeader.AABBMin = min(MLIClusterHeader.AABBMin, MLIChildClusterHeader.AABBMin);
    MLIClusterHeader.AABBMax = max(MLIClusterHeader.AABBMax, MLIChildClusterHeader.AABBMax);
    WeightedNormal += float3(
        UnpackNormal(MLIChildClusterHeader.WeightedNormal)
    ) * MLIChildClusterHeader.TotalIntensity;
    MLIClusterHeader.TotalIntensity += MLIChildClusterHeader.TotalIntensity;
    MLIClusterHeader.TotalArea += MLIChildClusterHeader.TotalArea;
    // Store WeightedNormal ^ 2 first. A separate finalization will be performed later.
    MLIClusterHeader.WeightedNormalVariance += MLIChildClusterHeader.WeightedNormalVariance;
    IntensityWeight = MLIChildClusterHeader.TotalIntensity;
}

void AccumulateMeshLightInstanceClusterNodeDataFromClusterChild_External (
    MeshLightClusterChild ClusterChild,
    MeshLight ML,
    uint MeshLightInstanceClusterNodeOffset,
    float3x4 RenderableToWorld,
    inout MeshLightInstanceClusterHeader MLIClusterHeader,
    inout float3 WeightedNormal, out float IntensityWeight
) {
    uint MeshLightLocalClusterIndex = ClusterChild.Index();
    uint MeshLightClusterIndex = ML.ClusterOffset + MeshLightLocalClusterIndex;
    MeshLightClusterHeader MLClusterHeader = LCH_MeshLightClusterHeaderBuffer[MeshLightClusterIndex];
    MeshLightInstanceClusterHeader MLIChildClusterHeader = LCH_RWMeshLightInstanceClusterHeaderBuffer[
        MeshLightInstanceClusterNodeOffset + MeshLightLocalClusterIndex
    ];
    AccumulateMeshLightInstanceClusterNodeDataFromClusterChild(
        ClusterChild, ML, MLIChildClusterHeader, RenderableToWorld,
        MLIClusterHeader, WeightedNormal, IntensityWeight
    );
}

// Dispatched via multi-draw-indirect. PROCESSING_LEVELS_PER_DISPATCH levels a one time

// Each thread is responsible for duplicating & building a subtree of depth PROCESSING_LEVELS_PER_DISPATCH
// For example, with PROCESSING_LEVELS_PER_DISPATCH = 3, each thread will build a subtree of depth 3 (4 - 2 - 1 nodes)
// from bottom to the top. Duplicating them from ML cluster nodes to MLI cluster nodes with identical hierarchy and
// recomputed headers based on different renderable transforms.
void LightPrecomputation_Level (uint VertexID : SV_VertexID, uint InstanceID : SV_InstanceID) {
    uint MeshLightInstanceIndex = LightGrid_ActiveMeshLightInstanceIndexBuffer[InstanceID];
    uint MLLevelLocalRootClusterIndex = VertexID;
    MeshLightInstance MLI = LCH_MeshLightInstanceBuffer[MeshLightInstanceIndex];
    MeshLight ML = LCH_MeshLightBuffer[MLI.MeshLightIndex];
    uint MeshLightClusterOffset = ML.ClusterOffset;
    uint MLLevelCount = ML.NumLevels;
    // The mesh light have no such level
    if (LightPrecomputation_LevelUB.LevelIndex >= MLLevelCount) return;
    uint RootLevelHeaderIndex = ML.LevelOffset + LightPrecomputation_LevelUB.LevelIndex;
    MeshLightLevelHeader RootLevelHeader = LCH_MeshLightLevelHeaderBuffer[RootLevelHeaderIndex];
    // Okay, finally we get the ML cluster index we want to preprocess in this dispatch.
    uint MLLocalRootClusterIndex = RootLevelHeader.ClusterOffset + MLLevelLocalRootClusterIndex;
    uint MLRootClusterIndex = MeshLightClusterOffset + MLLocalRootClusterIndex;
    // Get the MLI cluster index we want to write to.
    uint MLIRootClusterIndex = MLI.MeshLightInstanceClusterOffset.Offset() + MLLocalRootClusterIndex;

    float3x4 RenderableToWorld = RenderableTransformBuffer[MLI.RenderableIndex];
    
    StaticMeshHeader MeshHeader = StaticMeshHeaderBuffer[ML.StaticMeshIndex];
    uint DescriptionIndex = MeshHeader.DescriptionOffset + ML.StaticMeshDescriptionIndex;
    uint GeometryIndex = StaticMeshDescriptionBuffer[DescriptionIndex].x;
    GeometryHeader MeshGeometry = GeometryHeaderBuffer[GeometryIndex];

    MeshLightClusterChild MLLocalClusterNodeIndexBuffer[(1 << (PROCESSING_LEVELS_PER_DISPATCH + 1)) - 1];
    // Load root node.
    MLLocalClusterNodeIndexBuffer[0] = MakeMeshLightClusterChild(false, MLLocalRootClusterIndex);
    // Fill local subtree ML cluster node indices (load N+1 levels).
    for(int CurrentSubtreeLevel = 0; CurrentSubtreeLevel < PROCESSING_LEVELS_PER_DISPATCH; CurrentSubtreeLevel++) {
        int CurrentSubtreeLevelStart = (1u << CurrentSubtreeLevel) - 1;
        int CurrentSubtreeLevelCount = 1u << CurrentSubtreeLevel;
        for(int i = 0; i < CurrentSubtreeLevelCount; i++) {
            int TreeIndex = CurrentSubtreeLevelStart + i;
            MeshLightClusterChild MLLocalClusterIndex = MLLocalClusterNodeIndexBuffer[TreeIndex];
            if(IsMeshLightClusterChildValid(MLLocalClusterIndex)) {
                MeshLightClusterNode Node = LCH_MeshLightClusterNodeBuffer[MeshLightClusterOffset + MLLocalClusterIndex.Index()];
                MLLocalClusterNodeIndexBuffer[TreeIndex * 2 + 1] = Node.L;
                MLLocalClusterNodeIndexBuffer[TreeIndex * 2 + 2] = Node.R;
            } else {
                MLLocalClusterNodeIndexBuffer[TreeIndex * 2 + 1] = MakeInvalidMeshLightClusterChild();
                MLLocalClusterNodeIndexBuffer[TreeIndex * 2 + 2] = MakeInvalidMeshLightClusterChild();
            }
        }
    }
    // Cache thread local states from last level. We do not need to access memory for inner node computations.
    // For example, a subtree of depth 3: depth 0 (root): 1 node, 1: 2 nodes, 2: 4 (locally cached), 3: 8 nodes(external)
    MeshLightInstanceClusterHeader LocalMLIClusterHeaderBuffer[(1 << (PROCESSING_LEVELS_PER_DISPATCH-1))];
    // Bottom-top precomputation
    for(int CurrentSubtreeLevel = PROCESSING_LEVELS_PER_DISPATCH - 1; CurrentSubtreeLevel >= 0; CurrentSubtreeLevel--) {
        int CurrentSubtreeLevelStart = (1u << CurrentSubtreeLevel) - 1;
        int CurrentSubtreeLevelCount = 1u << CurrentSubtreeLevel;

        int CurrentTreeLevel = LightPrecomputation_LevelUB.LevelIndex + CurrentSubtreeLevel;

        for(int i = 0; i < CurrentSubtreeLevelCount; i++) {
            int TreeIndex = CurrentSubtreeLevelStart + i;
            MeshLightClusterChild MLLocalClusterIndex = MLLocalClusterNodeIndexBuffer[TreeIndex];
            // Only process existing nodes.
            if(IsMeshLightClusterChildValid(MLLocalClusterIndex)) {
                float L_Weight = 0, R_Weight = 0;
                MeshLightClusterHeader MLClusterHeader = LCH_MeshLightClusterHeaderBuffer[MeshLightClusterOffset + MLLocalClusterIndex.Index()];
                MeshLightInstanceClusterHeader MLIClusterHeader = (MeshLightInstanceClusterHeader)0;
                MLIClusterHeader.AABBMin = float3(FLT_MAX, FLT_MAX, FLT_MAX);
                MLIClusterHeader.AABBMax = float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
                float3 WeightedNormal = 0;
                MLIClusterHeader.MeshLightInstanceIndex = MeshLightInstanceIndex;
                MLIClusterHeader.WeightedNormalVariance = 0;
                MLIClusterHeader.TotalIntensity = 0;
                MLIClusterHeader.TotalArea = 0;
                MLIClusterHeader.Hash = MLClusterHeader.Hash; // Duplicate the hash from the ML cluster header
                // Ready to accumulate data from children or triangles
                MeshLightClusterChild LeftChild = MLLocalClusterNodeIndexBuffer[TreeIndex * 2 + 1];
                MeshLightClusterChild RightChild = MLLocalClusterNodeIndexBuffer[TreeIndex * 2 + 2];
                if(IsMeshLightClusterChildValid(LeftChild)) {
                    // Left child
                    if(LeftChild.bIsLeaf()) {
                        // Leaf node, interpret as triangle index
                        AccumulateMeshLightInstanceClusterNodeDataFromTriangleChild(
                            LeftChild, ML, MLI, MeshGeometry, RenderableToWorld,
                            MLIClusterHeader, WeightedNormal, L_Weight
                        );
                    } else {
                        // Inner node, interpret as cluster index
                        if(CurrentSubtreeLevel == PROCESSING_LEVELS_PER_DISPATCH - 1) {
                            // Child is not present in the local buffer, load directly from the global cluster header buffer.
                            AccumulateMeshLightInstanceClusterNodeDataFromClusterChild_External(
                                LeftChild, ML, MLI.MeshLightInstanceClusterOffset.Offset(),
                                RenderableToWorld, MLIClusterHeader, WeightedNormal, L_Weight
                            );
                        } else {
                            // Child is present in the local buffer, load from the local buffer
                            AccumulateMeshLightInstanceClusterNodeDataFromClusterChild(
                                LeftChild, ML, LocalMLIClusterHeaderBuffer[i<<1], RenderableToWorld,
                                MLIClusterHeader, WeightedNormal, L_Weight
                            );
                        }
                    }
                }
                if(IsMeshLightClusterChildValid(RightChild)) {
                    // Right child
                    if(RightChild.bIsLeaf()) {
                        // Leaf node, interpret as triangle index
                        AccumulateMeshLightInstanceClusterNodeDataFromTriangleChild(
                            RightChild, ML, MLI, MeshGeometry, RenderableToWorld,
                            MLIClusterHeader, WeightedNormal, R_Weight
                        );
                    } else {
                        // Inner node, interpret as cluster index
                        if(CurrentSubtreeLevel == PROCESSING_LEVELS_PER_DISPATCH - 1) {
                            AccumulateMeshLightInstanceClusterNodeDataFromClusterChild_External(
                                RightChild, ML, MLI.MeshLightInstanceClusterOffset.Offset(),
                                RenderableToWorld, MLIClusterHeader, WeightedNormal, R_Weight
                            );
                        } else {
                            AccumulateMeshLightInstanceClusterNodeDataFromClusterChild(
                                RightChild, ML, LocalMLIClusterHeaderBuffer[(i<<1) + 1],
                                RenderableToWorld, MLIClusterHeader, WeightedNormal, R_Weight
                            );
                        }
                    }
                }
                // Finalize some of the the cluster header dataif
                MLIClusterHeader.WeightedNormal = PackNormal(SafeNormalize(WeightedNormal));
                // WeightedNormal ^ 2 is finalized in a separate shader. Not here.

                // Write back to the instance cluster header buffer.
                // (Duplicate the same tree hierarchy in ML cluster buffers to MLI cluster buffers for each instance.)
                uint MLIClusterIndex = MLI.MeshLightInstanceClusterOffset.Offset() + MLLocalClusterIndex.Index();
                LCH_RWMeshLightInstanceClusterHeaderBuffer[MLIClusterIndex] = MLIClusterHeader;
                // Also, update the local header buffer for the next level of processing
                LocalMLIClusterHeaderBuffer[i] = MLIClusterHeader;

                // Build sampling probabilities
                MeshLightInstanceClusterNode MLINode = (MeshLightInstanceClusterNode)0;
                float eps = max(0.01 * (L_Weight + R_Weight), 1e-7f);
                MLINode.L_Probability = (L_Weight + eps) / (L_Weight + R_Weight + 2 * eps);
                LCH_RWMeshLightInstanceClusterNodeBuffer[MLIClusterIndex] = MLINode;
            }
        }
    }
}

void LightPrecomputation_FinalizeMLIClusters (uint VertexID : SV_VertexID, uint InstanceID : SV_InstanceID) {
    uint MeshLightInstanceIndex = LightGrid_ActiveMeshLightInstanceIndexBuffer[InstanceID];
    MeshLightInstance MLI = LCH_MeshLightInstanceBuffer[MeshLightInstanceIndex];
    if(MLI.MeshLightInstanceClusterOffset.bIsTriangle()) return;
    uint MeshLightInstanceClusterIndex = MLI.MeshLightInstanceClusterOffset.Offset() + VertexID;
    // Simply finalize the WeightedNormalVariance by performing sqrt(WeightedNormalVariance - WeightedNormal^2). This is based on the fact that we stored WeightedNormal^2 in the variance field before finalization.
    MeshLightInstanceClusterHeader MLIClusterHeader = LCH_RWMeshLightInstanceClusterHeaderBuffer[MeshLightInstanceClusterIndex];
    float3 WeightedNormal = UnpackNormal(MLIClusterHeader.WeightedNormal) * MLIClusterHeader.TotalIntensity;
    LCH_RWMeshLightInstanceClusterHeaderBuffer[MeshLightInstanceClusterIndex].WeightedNormalVariance 
        = sqrt(max(MLIClusterHeader.WeightedNormalVariance - dot(WeightedNormal, WeightedNormal), 0));
}

#ifndef MAX_NUM_GRID_LIGHTS
#define MAX_NUM_GRID_LIGHTS 8
#endif

#define MAX_NUM_SUBDIVIDED_LIGHTS_PER_MLI 8

struct SubdividedLightsForGrid {
    uint NumElements;
    MeshLightInstanceElementOffset MLILocalElements[MAX_NUM_SUBDIVIDED_LIGHTS_PER_MLI];
};

// Subdivide a MLI according to grid pressure.
// TODO Move the random dropping logic from outside to inside (here) according to light weight
// estimates dynamically during subdivision. For example, we can stop subdivision early and 
// drop some of the subtree. This can lead to better results than the current approach which 
// performs subdivision first and then random dropping.
SubdividedLightsForGrid SubdivideMLIForGridAndUpdateGridPressure (MeshLight ML, MeshLightInstance MLI, inout LightGridPressureContext PressureContext) {
    SubdividedLightsForGrid Subdivided = (SubdividedLightsForGrid)0;
    Subdivided.MLILocalElements[0] = MakeMeshLightInstanceElementOffset(
        MLI.MeshLightInstanceClusterOffset.bIsTriangle(),
        0 // We meant to use local offsets relative to the MLI cluster offset, so the root cluster is always at offset 0
    ); // Start from the root cluster
    Subdivided.NumElements = 1;
    PressureContext.CurrentNumLights ++; // A new light is sampled
    uint MLIClusterOffset = MLI.MeshLightInstanceClusterOffset.Offset();
    uint MLClusterOffset = ML.ClusterOffset;
    while(true) {
        bool bSubdivided = false;
        uint PrevNodeCount = Subdivided.NumElements;
        for(int i = 0; i < PrevNodeCount; i++) {
            MeshLightInstanceElementOffset Element = Subdivided.MLILocalElements[i];
            if(Element.bIsTriangle()) {
                // Reached triangle level, stop subdivision
                continue;
            }

            // Found a candidate for subdivision, set bCanFurtherSubdivide in the pressure context to true
            PressureContext.bCanFurtherSubdivide = true;
            
            // Check if the node is large enough for splitting
            MeshLightInstanceClusterHeader MLIClusterHeader = LCH_MeshLightInstanceClusterHeaderBuffer[MLIClusterOffset + Element.Offset()];
            if(MLIClusterHeader.TotalIntensity < PressureContext.IntensityThreshold) {
                // Skip small nodes for subdivision asfasdf
                continue ;
            }
            bSubdivided = true;
            MeshLightClusterNode Node = LCH_MeshLightClusterNodeBuffer[MLClusterOffset + Element.Offset()];
            // Inner node, add children to the list
            // Overwrite the current node with the left child
            Subdivided.MLILocalElements[i] = MakeMeshLightInstanceElementOffset(Node.L.bIsLeaf(), Node.L.Index());
            // Add the right child to the end of the list
            Subdivided.MLILocalElements[Subdivided.NumElements++] = MakeMeshLightInstanceElementOffset(Node.R.bIsLeaf(), Node.R.Index());
            // Update the current number of lights for the grid (1->2 splitting)
            PressureContext.CurrentNumLights ++;
            if(Subdivided.NumElements >= MAX_NUM_SUBDIVIDED_LIGHTS_PER_MLI) {
                // Reached the maximum number of nodes we can process for this grid, stop subdivision
                return Subdivided;
            }
        }
        if(!bSubdivided) {
            // No node is subdivided in this iteration, stop subdivision
            break;
        }
    }
    return Subdivided;
}

groupshared uint SharedListElementsRequired, SharedListOffsetBase;
[numthreads(WAVE_SIZE, 1, 1)]
void LightPrecomputation_InjectLights(uint DispatchID: SV_DispatchThreadID, uint LocalID : SV_GroupThreadID) {
    uint GridIndexListIndex = DispatchID;
    uint GridIndex1 = LightGrid_RWActiveGridIndicesBuffer[GridIndexListIndex];
    // x, y, z, cascade
    uint4 GridIndex = LightGrid_GetGridIndex(GridIndex1);
    float GridSize;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    // TODO This can still be slow when there are hundreds of MLIs in the scene. Find a better injection method for 
    // a larger number of instanced lights
    uint NumActiveLights = LightGrid_RWActiveMeshLightInstanceCount[0];
    uint NumGridSubdividedActiveLights = 0;
    uint NumGridLights = 0, WriteLocation = 0;
    // Double buffering for unbiased selection of a group of lights
    // Sampled: current selected, Candidate: new candidate group
    // Initialy the candidate group is behind the sampled group, so naturally the sampled group
    // is filled first.
    uint SampledOffset = 0, CandidateOffset = MAX_NUM_GRID_LIGHTS;
    Random R = MakeRandom(17419142u + DispatchID, LightStructure_UB.FrameIndex);
    float U = R.rand();
    float SumFilteredWeights = 0.0f, SumSampledWeights = 0.f, SumCandidateWeights = 0.f;
    float SumFilteredOutWeights = 0.f;

    float DynamicThreshold = LightStructure_UB.LightInjectionIntensityThreshold;
    // Temporary buffer to store light list indices for the current grid.
    // The first half is for sampled lights, the second half is for candidate lights.
    MeshLightInstanceElementOffset LocalGridLightMLIClusterSelection[MAX_NUM_GRID_LIGHTS * 2]; // Absolute buffer offsets.
    float LocalGridLightWeights[MAX_NUM_GRID_LIGHTS * 2]; 
    uint2 GridPressure = LightGrid_RWGridPressureBuffer[GridIndex1];
    LightGridPressureContext PressureContext = LightGridPressureContext_Init(GridPressure);
    SubdividedLightsForGrid Subdivided;
    uint NumValidMLILights = 0;
    // TODO better injection strategy (subdivide first, then inject?)
    for (uint ActiveMeshLightInstanceIndex = 0; ActiveMeshLightInstanceIndex < NumActiveLights; ActiveMeshLightInstanceIndex++) {
        uint MeshLightInstanceIndex = LightGrid_ActiveMeshLightInstanceIndexBuffer[ActiveMeshLightInstanceIndex];
        MeshLightInstance MLI = LCH_MeshLightInstanceBuffer[MeshLightInstanceIndex];
        if(!IsValid(MLI.MeshLightInstanceClusterOffset)) continue ;
        NumValidMLILights ++;
        MeshLight ML = LCH_MeshLightBuffer[MLI.MeshLightIndex];
        // For each MeshLightInstance, pick a good subdivision level based on its intensity and the density of lights (history).
        SubdividedLightsForGrid Subdivided = SubdivideMLIForGridAndUpdateGridPressure(ML, MLI, PressureContext);
        NumGridSubdividedActiveLights += Subdivided.NumElements;
        for(int i = 0; i < Subdivided.NumElements; i++) {
            float Weight = 0;
            MeshLightInstanceElementOffset SelectedAbsolute = (MeshLightInstanceElementOffset)0;
            MeshLightInstanceElementOffset ElementBeforeConversion = Subdivided.MLILocalElements[i];
            if(ElementBeforeConversion.bIsTriangle()) {
                uint AbsoluteOffset = MLI.MeshLightInstanceTriangleOffset + ElementBeforeConversion.Offset();
                MeshLightInstanceTriangle MLITriangle = LCH_MeshLightInstanceTriangleBuffer[AbsoluteOffset];
                SelectedAbsolute = MakeMeshLightInstanceElementOffset(true, AbsoluteOffset);
                Weight = LightGrid_EstimateLightGridPerceptualContribution(MLITriangle, GridMin, GridSize);
            } else {
                uint AbsoluteOffset = MLI.MeshLightInstanceClusterOffset.Offset() + ElementBeforeConversion.Offset();
                MeshLightInstanceClusterHeader MLICluster = LCH_MeshLightInstanceClusterHeaderBuffer[AbsoluteOffset];
                SelectedAbsolute = MakeMeshLightInstanceElementOffset(false, AbsoluteOffset);
                Weight = LightGrid_EstimateLightGridPerceptualContribution(MLICluster, GridMin, GridSize);
            }
            float LightCullingProbabilityMin = LightStructure_UB.LightCullingRate;
            // Lights with lower contribution have higher probability to be culled.
            float LightCullingProbability = LightCullingProbabilityMin + (1 - LightCullingProbabilityMin) * saturate(DynamicThreshold / max(Weight, 1e-6f));
            bool bIsLightAlive = R.rand() > LightCullingProbability;
            if (bIsLightAlive) {
                // Keep this light in the double buffer and accumulate weights depending on which
                // group it is in
                LocalGridLightMLIClusterSelection[WriteLocation] = SelectedAbsolute;
                LocalGridLightWeights[WriteLocation] = Weight;
                if (SampledOffset <= WriteLocation && WriteLocation < SampledOffset + MAX_NUM_GRID_LIGHTS)
                    SumSampledWeights += Weight;
                if (CandidateOffset <= WriteLocation && WriteLocation < CandidateOffset + MAX_NUM_GRID_LIGHTS)
                    SumCandidateWeights += Weight;
                SumFilteredWeights += Weight;
                WriteLocation++, NumGridLights++;
                if (WriteLocation == CandidateOffset + MAX_NUM_GRID_LIGHTS) {
                    // Candidate group is full, time to select which group to keep
                    float P = SumCandidateWeights / max(SumFilteredWeights, 1e-6f);
                    if (U < P) {
                        // Accept: replace the group with the candidate group
                        uint Temp = SampledOffset;
                        SampledOffset = CandidateOffset;
                        CandidateOffset = Temp;
                        SumSampledWeights = SumCandidateWeights;
                        U = U / P;
                    } else {
                        // Drop the candidate group
                        U = (U - P) / (1.00001f - P);
                    }
                    // Reset candidate statistics
                    SumCandidateWeights = 0;
                    // Replace write location to the start of the candidate group for refilling
                    WriteLocation = CandidateOffset;
                }
            } else {
                SumFilteredOutWeights += Weight;
            }
        }
    }
    // Update light grid pressure info
    {
        // The number of subdivided lights equals to the number of injected lights. Almost no any subdivision is performed.
        bool bNoSubdivision = NumValidMLILights == NumGridSubdividedActiveLights;
        bool bCanFurtherSubdivide = PressureContext.bCanFurtherSubdivide;
        if(!bNoSubdivision && NumGridSubdividedActiveLights >= MAX_NUM_GRID_LIGHTS) {
            // Increase the threshold for next time to reduce the subdivision and injection for this grid in the next frame,
            // which can help reduce the number of lights and thus the pressure for this grid.
            PressureContext.IntensityThreshold *= 1.31f;  
        } else if (bCanFurtherSubdivide && NumGridSubdividedActiveLights < MAX_NUM_GRID_LIGHTS / 2) {
            // Decrease the threshold for next time to increase the subdivision and injection for this grid in the next frame,
            // which can help increase the number of lights and thus the accuracy for this grid.
            PressureContext.IntensityThreshold *= 0.72f;
        }
    }
    LightGrid_RWGridPressureBuffer[GridIndex1] = LightGrid_UpdateGridPressure(PressureContext);
    uint NumSampledLights = min(NumGridLights, MAX_NUM_GRID_LIGHTS);
    if (WriteLocation > CandidateOffset && WriteLocation < CandidateOffset + MAX_NUM_GRID_LIGHTS) {
        // Final swapping if the sampled group is full and the candidate group is partially filled
        float P = SumCandidateWeights / (SumFilteredWeights + 1e-6f);
        if (U < P) { // Replace the group with the candidate group
            // Replace the light count with the number of candidate lights
            NumSampledLights = WriteLocation - CandidateOffset;
            // Replace sampled offset with candidate offset
            uint Temp = SampledOffset;
            SampledOffset = CandidateOffset;
            CandidateOffset = Temp;
            SumSampledWeights = SumCandidateWeights;
        }
    }
    // Get ready for the final list
    MeshLightInstanceElementOffset FinalSampledClusters[MAX_NUM_GRID_LIGHTS];
    float FinalSampledWeights[MAX_NUM_GRID_LIGHTS];
    for(int i = 0; i < NumSampledLights; i++) {
        FinalSampledClusters[i] = LocalGridLightMLIClusterSelection[SampledOffset + i];
        FinalSampledWeights[i] = LocalGridLightWeights[SampledOffset + i];
    }

    // Write to grid
    LightGrid_RWGridLightListLengthBuffer[GridIndex1] = NumSampledLights;
    // Account for culled lights by normalizing the total weight, reducing bias.
    // (Cdf does not involve in RIS process, but involves in the correction with the final lighting computation)
    float SumWeights = SumFilteredWeights + SumFilteredOutWeights;
    LightGrid_RWGridLightListCdfBuffer[GridIndex1] = SumSampledWeights / max(SumWeights, 1e-6f);
    if (LocalID == 0) SharedListElementsRequired = 0;
    GroupMemoryBarrierWithGroupSync();
    uint LocalOffset = 0;
    InterlockedAdd(SharedListElementsRequired, NumSampledLights, LocalOffset);
    GroupMemoryBarrierWithGroupSync();
    if (LocalID == 0) {
        InterlockedAdd(LightGrid_RWListAllocator[0], SharedListElementsRequired, SharedListOffsetBase);
    }
    GroupMemoryBarrierWithGroupSync();
    uint GlobalOffset = SharedListOffsetBase + LocalOffset;
    uint NumWritableLights = NumSampledLights;
    uint MaxNumEntries = max(LightStructure_UB.LightGridMaxNumEntries, 1u);
    if (GlobalOffset >= MaxNumEntries) {
        NumWritableLights = 0;
    } else {
        NumWritableLights = min(NumWritableLights, MaxNumEntries - GlobalOffset);
    }
    LightGrid_RWGridLightListOffsetBuffer[GridIndex1] = GlobalOffset;
    LightGrid_RWGridLightListLengthBuffer[GridIndex1] = NumWritableLights;
    for (uint i = 0; i < NumWritableLights; i++) {
        MeshLightInstanceElementOffset Value = FinalSampledClusters[i];
        // This time we store active light list index in the grid buffer 
        LightGrid_RWListMeshLightInstanceElementIndexBuffer[GlobalOffset + i] = Value;
    }
}
