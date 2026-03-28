// The shader performs light precomputation & injection for all important scene lights

#include "headers/Random.hlsl"
#include "resources/StaticMeshResources.hlsl"
#include "resources/LightClusterHierarchyResources.hlsl"
#include "resources/LightGrid.hlsl"


void LightPrecomputation_Triangle (uint VertexID : SV_VertexID, uint InstanceID : SV_InstanceID) {
    uint MeshLightLocalTriangleIndex = VertexID;
    uint MeshLightInstanceIndex = InstanceID;
    
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
    uint i0 = IndexBuffer[GeometryTriangleIndexOffset];
    uint i1 = IndexBuffer[GeometryTriangleIndexOffset + 1];
    uint i2 = IndexBuffer[GeometryTriangleIndexOffset + 2];
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
    MLITriangle.Hash = LCH_MeshLightTriangleHashBuffer[MeshLightTriangleIndex].Hash;
    // Write to output buffer.
    LCH_RWMeshLightInstanceTriangleBuffer[MeshLightInstanceTriangleIndex] = MLITriangle;
}

// Each thread process 4 - 2 - 1 (PROCESSING_LEVELS_PER_DISPATCH) nodes in the hierarchy. The total number of dispatches is ceil(MaxDepth / PROCESSING_LEVELS_PER_DISPATCH).
#define PROCESSING_LEVELS_PER_DISPATCH 3

struct LightPrecomputationLevelUB {
    uint LevelIndex;
    uint MeshLightInstanceCount;
    uint Padding[2];
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
    uint MeshLightLocalTriangleIndex = TriangleChild.Index;
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
    uint MeshLightLocalClusterIndex = ClusterChild.Index;
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
    uint MeshLightInstanceIndex = InstanceID;
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
    uint MLIRootClusterIndex = MLI.MeshLightInstanceClusterOffset.Offset + MLLocalRootClusterIndex;

    float3x4 RenderableToWorld = RenderableTransformBuffer[MLI.RenderableIndex];
    
    StaticMeshHeader MeshHeader = StaticMeshHeaderBuffer[ML.StaticMeshIndex];
    uint DescriptionIndex = MeshHeader.DescriptionOffset + ML.StaticMeshDescriptionIndex;
    uint GeometryIndex = StaticMeshDescriptionBuffer[DescriptionIndex].y;
    GeometryHeader MeshGeometry = GeometryHeaderBuffer[GeometryIndex];

    MeshLightClusterChild MLLocalClusterNodeIndexBuffer[(1 << (PROCESSING_LEVELS_PER_DISPATCH + 1)) - 1];
    // Load root node.
    MLLocalClusterNodeIndexBuffer[0] = MakeMeshLightClusterChild(false, MLLocalRootClusterIndex);
    // Fill local subtree ML cluster node indices (load N+1 levels).
    for(int CurrentSubtreeLevel = 0; CurrentSubtreeLevel < PROCESSING_LEVELS_PER_DISPATCH; CurrentSubtreeLevel++) {
        int CurrentSubtreeLevelStart = (1 << CurrentSubtreeLevel) - 1;
        int CurrentSubtreeLevelCount = 1 << CurrentSubtreeLevel;
        for(int i = 0; i < CurrentSubtreeLevelCount; i++) {
            int TreeIndex = CurrentSubtreeLevelStart + i;
            MeshLightClusterChild MLLocalClusterIndex = MLLocalClusterNodeIndexBuffer[TreeIndex];
            if(IsMeshLightClusterChildValid(MLLocalClusterIndex)) {
                MeshLightClusterNode Node = LCH_MeshLightClusterNodeBuffer[MeshLightClusterOffset + MLLocalClusterIndex.Index];
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
        int CurrentSubtreeLevelStart = (1 << CurrentSubtreeLevel) - 1;
        int CurrentSubtreeLevelCount = 1 << CurrentSubtreeLevel;

        int CurrentTreeLevel = LightPrecomputation_LevelUB.LevelIndex + CurrentSubtreeLevel;

        for(int i = 0; i < CurrentSubtreeLevelCount; i++) {
            int TreeIndex = CurrentSubtreeLevelStart + i;
            MeshLightClusterChild MLLocalClusterIndex = MLLocalClusterNodeIndexBuffer[TreeIndex];
            // Only process existing nodes.
            if(IsMeshLightClusterChildValid(MLLocalClusterIndex)) {
                float L_Weight = 0, R_Weight = 0;
                MeshLightClusterHeader MLClusterHeader = LCH_MeshLightClusterHeaderBuffer[MeshLightClusterOffset + MLLocalClusterIndex.Index];
                MeshLightInstanceClusterHeader MLIClusterHeader = (MeshLightInstanceClusterHeader)0;
                MLIClusterHeader.AABBMin = float3(FLT_MAX, FLT_MAX, FLT_MAX);
                MLIClusterHeader.AABBMax = float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
                float3 WeightedNormal = 0;
                MLIClusterHeader.WeightedNormalVariance = 0;
                MLIClusterHeader.TotalIntensity = 0;
                MLIClusterHeader.TotalArea = 0;
                MLIClusterHeader.Hash = MLClusterHeader.Hash; // Duplicate the hash from the ML cluster header
                // Ready to accumulate data from children or triangles
                MeshLightClusterChild LeftChild = MLLocalClusterNodeIndexBuffer[TreeIndex * 2 + 1];
                MeshLightClusterChild RightChild = MLLocalClusterNodeIndexBuffer[TreeIndex * 2 + 2];
                if(IsMeshLightClusterChildValid(LeftChild)) {
                    // Left child
                    if(LeftChild.bIsLeaf) {
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
                                LeftChild, ML, MLI.MeshLightInstanceClusterOffset.Offset,
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
                    if(RightChild.bIsLeaf) {
                        // Leaf node, interpret as triangle index
                        AccumulateMeshLightInstanceClusterNodeDataFromTriangleChild(
                            RightChild, ML, MLI, MeshGeometry, RenderableToWorld,
                            MLIClusterHeader, WeightedNormal, R_Weight
                        );
                    } else {
                        // Inner node, interpret as cluster index
                        if(CurrentSubtreeLevel == PROCESSING_LEVELS_PER_DISPATCH - 1) {
                            AccumulateMeshLightInstanceClusterNodeDataFromClusterChild_External(
                                RightChild, ML, MLI.MeshLightInstanceClusterOffset.Offset,
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
                // Finalize some of the the cluster header data
                MLIClusterHeader.WeightedNormal = PackNormal(normalize(WeightedNormal));
                // WeightedNormal ^ 2 is finalized in a separate shader. Not here.

                // Write back to the instance cluster header buffer.
                // (Duplicate the same tree hierarchy in ML cluster buffers to MLI cluster buffers for each instance.)
                uint MLIClusterIndex = MLI.MeshLightInstanceClusterOffset.Offset + MLLocalClusterIndex.Index;
                LCH_MeshLightInstanceClusterHeaderBuffer[MLIClusterIndex] = MLIClusterHeader;

                // Build sampling probabilities
                MeshLightInstanceClusterNode MLINode = (MeshLightInstanceClusterNode)0;
                float eps = min(0.01 * (L_Weight + R_Weight), 1e-7f);
                MLINode.L_Probability = (L_Weight + eps) / (L_Weight + R_Weight + 2 * eps);
                LCH_RWMeshLightInstanceClusterNodeBuffer[MLIClusterIndex] = MLINode;
            }
        }
    }
}

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void LightPrecomputation_FinalizeMLIClusters (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID >= MLINodeCount) return;
    // Simply finalize the WeightedNormalVariance by performing sqrt(WeightedNormalVariance - WeightedNormal^2). This is based on the fact that we stored WeightedNormal^2 in the variance field before finalization.
    MeshLightInstanceClusterHeader MLIClusterHeader = LCH_RWMeshLightInstanceClusterHeaderBuffer[DispatchID];
    float3 WeightedNormal = UnpackNormal(MLIClusterHeader.WeightedNormal) * MLIClusterHeader.TotalIntensity;
    LCH_RWMeshLightInstanceClusterHeaderBuffer[DispatchID].WeightedNormalVariance 
        = sqrt(max(MLIClusterHeader.WeightedNormalVariance - dot(WeightedNormal, WeightedNormal), 0));
}

StructuredBuffer<uint> LightGrid_ActiveGridIndicesBuffer;
StructuredBuffer<uint> LightGrid_ActiveMeshLightInstanceCount;

#ifndef MAX_NUM_GRID_LIGHTS
#define MAX_NUM_GRID_LIGHTS 8
#endif

groupshared uint SharedListElementsRequired, SharedListOffsetBase;
[numthreads(WAVE_SIZE, 1, 1)]
void LightPrecomputation_InjectLights(uint DispatchID: SV_DispatchThreadID, uint LocalID : SV_GroupThreadID) {
    uint GridIndexListIndex = DispatchID;
    uint GridIndex1 = LightGrid_ActiveGridIndicesBuffer[GridIndexListIndex];
    // x, y, z, cascade
    uint4 GridIndex = LightGrid_GetGridIndex(GridIndex1);
    float GridSize;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    // TODO use multi level injection for a larger number of instanced lights
    uint NumActiveLights = LightGrid_ActiveMeshLightInstanceCount[0];
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
    // Temporary buffer to store light list indices for the current grid. The first half is for sampled lights, the second half is for candidate lights.
    MeshLightClusterChild LocalGridLightMLIClusterSelection[MAX_NUM_GRID_LIGHTS * 2]; 
    float LocalGridLightWeights[MAX_NUM_GRID_LIGHTS * 2]; 
    // TODO better injection strategy (subdivide first, then inject?)
    for (uint MeshLightInstanceIndex = 0; MeshLightInstanceIndex < NumActiveLights; MeshLightInstanceIndex++) {
        MeshLightInstance MLI = LCH_MeshLightInstanceBuffer[MeshLightInstanceIndex];
        if(!IsValid(MLI.MeshLightInstanceClusterOffset)) continue ;
        // For each MeshLightInstance, pick a good subdivision level based on its intensity and the density of lights (history).
        uint SubdivisionLevel = TODO;
        MeshLight ML = LCH_MeshLightBuffer[MLI.MeshLightIndex];
        SubdivisionLevel = min(SubdivisionLevel, ML.NumLevels);
        uint ClusterCount = 0;
        // The offset to read from the MLI cluster/triangle buffer. 
        // It can be either the triangle offset or the cluster offset depending on whether we use triangle or cluster for injection.
        uint SrcOffset = 0;
        // The level cluster offset in local MLI if the injection source is a cluster node.
        uint LevelClusterOffset = 0;
        bool bUseTriangle = false;
        if(SubdivisionLevel == ML.NumLevels)
        {
            bUseTriangle = true;
        }
        if(bUseTriangle && SubdivisionLevel == 0) {
            // A cluster with a single triangle.
            ClusterCount = 1;
            SrcOffset    = MLI.MeshLightInstanceClusterOffset.Offset;
        } else {
            if(bUseTriangle) SubdivisionLevel --;
            MeshLightLevelHeader LevelHeader = LCH_MeshLightLevelHeaderBuffer[ML.LevelOffset + SubdivisionLevel];
            ClusterCount  = LevelHeader.ClusterCount;
            SrcOffset     = MLI.MeshLightInstanceClusterOffset.Offset + LevelHeader.ClusterOffset;
        }
        
        for(int i = 0; i < ClusterCount; i++) {
            float Weight = 0;
            MeshLightClusterChild Selected;
            if(bUseTriangle) {
                MeshLightInstanceTriangle MLITriangle = LCH_MeshLightInstanceTriangleBuffer[
                    SrcOffset + i
                ];
                Selected = MakeMeshLightClusterChild(true, i);
                Weight = LightGrid_EstimateLightGridPerceptualContribution(MLITriangle, GridMin, GridSize);
            } else {
                MeshLightInstanceClusterHeader MLICluster = LCH_MeshLightInstanceClusterHeaderBuffer[
                    SrcOffset + i
                ];
                Selected = MakeMeshLightClusterChild(false, LevelClusterOffset + i);
                Weight = LightGrid_EstimateLightGridPerceptualContribution(MLICluster, GridMin, GridSize);
            }
            float LightCullingProbabilityMin = LightStructure_UB.LightCullingRate;
            // Lights with lower contribution have higher probability to be culled.
            float LightCullingProbability = LightCullingProbabilityMin + (1 - LightCullingProbabilityMin) * saturate(DynamicThreshold / max(Weight, 1e-6f));
            bool bIsLightAlive = R.rand() > LightCullingProbability;
            if (bIsLightAlive) {
                // Keep this light in the double buffer and accumulate weights depending on which
                // group it is in
                LocalGridLightMLIClusterSelection[WriteLocation] = Selected;
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
    // It's mathematically incorrect to include filtered out weights here
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
        MeshLightClusterChild Value = FinalSampledClusters[i];
        // This time we store active light list index in the grid buffer 
        LightGrid_RWListActiveLightListIndexBuffer[GlobalOffset + i] = Value;
    }
}

// Precompute lights, filter active lights and gather light data for later injection
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void PrecomputeLights(uint DispatchID: SV_DispatchThreadID) {
    uint LightIndex = DispatchID;
    if (LightIndex >= LightStructure_UB.MaxNumLights) return;
    AreaLight LightData = LightBuffer[LightIndex];
    if (LightData.Flags == 0) return; // Invalid light, skip
    // Extract light data
    bool bActive;
    EvaluatedAreaLight Evaluated = EvaluateLight(LightData, bActive);
    // Evaluate other features
    float3 N = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    // Precompute
    if(bActive) {
        PrecomputedLight L = (PrecomputedLight)0;
        L.V0 = Evaluated.V0;
        L.V1 = Evaluated.V1;
        L.V2 = Evaluated.V2;
        L.Normal = N;
        L.Hash = GetExpandedLightHash64(LightIndex, GetLightHash32(LightData));
        float Area = length(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0)) * 0.5f;
        L.Intensity = RadianceToLuminance(Evaluated.EstimatedAverageEmission) * Area;
        if (L.Intensity > 1e-3f) {
            // Allocate active light list
            uint WaveNumActiveLights = WaveActiveCountBits(true);
            uint WaveLightListOffset = 0;
            if (WaveIsFirstLane()) {
                InterlockedAdd(LightGrid_RWActiveLightListCount[0], WaveNumActiveLights, WaveLightListOffset);
            }
            WaveLightListOffset = WaveReadLaneFirst(WaveLightListOffset);
            uint WaveLightListIndex = WavePrefixCountBits(true);
            uint LightListIndex = WaveLightListOffset + WaveLightListIndex;
            // Precompute and store active lights
            LightGrid_RWActiveLightListBuffer[LightListIndex] = LightIndex;
            LightGrid_RWPrecomputedActiveLightBuffer[LightListIndex] = PackPrecomputedLight(L);
        }
    }
}

// Dispatch a thread for each grid
groupshared uint SharedListElementsRequired, SharedListOffsetBase;
groupshared uint SharedGridLightListIndices[WAVE_SIZE * MAX_NUM_GRID_LIGHTS * 2];
[numthreads(WAVE_SIZE, 1, 1)]
void InjectLights(uint DispatchID: SV_DispatchThreadID, uint LocalID : SV_GroupThreadID) {
    if (DispatchID >= LightStructure_UB.LightGridNumGrids) return;
    uint GridIndex1 = DispatchID;
    // x, y, z, cascade
    uint4 GridIndex = LightGrid_GetGridIndex(GridIndex1);
    float GridSize;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    // TODO use multi level injection for a large number of lights
    uint NumActiveLights = LightGrid_RWActiveLightListCount[0];
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

    // TODO this still introduces a lot of noise upon overflowing. Need a better strategy.

    float DynamicThreshold = LightStructure_UB.LightInjectionIntensityThreshold;
    for (uint LightListIndex = 0; LightListIndex < NumActiveLights; LightListIndex++) {
        PrecomputedLight L = UnpackPrecomputedLight(LightGrid_RWPrecomputedActiveLightBuffer[LightListIndex]);
        float Weight = LightGrid_EstimateLightGridPerceptualContribution(L, GridMin, GridSize);
        float LightCullingProbabilityMin = LightStructure_UB.LightCullingRate;
        // Lights with lower contribution have higher probability to be culled.
        float LightCullingProbability = LightCullingProbabilityMin + (1 - LightCullingProbabilityMin) * saturate(DynamicThreshold / max(Weight, 1e-6f));
        bool bIsLightAlive = R.rand() > LightCullingProbability;
        if (bIsLightAlive) {
            // Keep this light in the double buffer and accumulate weights depending on which
            // group it is in
            SharedGridLightListIndices[WriteLocation * WAVE_SIZE + LocalID] = LightListIndex;
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
    // Write to grid
    LightGrid_RWGridLightListLengthBuffer[GridIndex1] = NumSampledLights;
    // It's mathematically incorrect to include filtered out weights here
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
        uint LightListIndex = SharedGridLightListIndices[(SampledOffset + i) * WAVE_SIZE + LocalID];
        // This time we store active light list index in the grid buffer 
        LightGrid_RWListActiveLightListIndexBuffer[GlobalOffset + i] = LightListIndex;
    }
}