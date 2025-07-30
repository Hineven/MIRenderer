#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedLight.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Math.hlsl"
#include "headers/Radiometry.hlsl"
#include "headers/Random.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Light.hlsl"
#include "headers/ScreenSpaceRayTracing.hlsl"
#include "headers/HybridTracing.hlsl"
#include "resources/BindlessTextureResources.hlsl"

// Input macros
#ifndef MAX_NUM_GRID_LIGHTS
#define MAX_NUM_GRID_LIGHTS 32
#endif
#ifndef LIGHT_GRID_NUM_CASCADES
#define LIGHT_GRID_NUM_CASCADES 6
#endif

#ifndef NUM_LIGHT_SAMPELR_SAMPLES
#define NUM_LIGHT_SAMPELR_SAMPLES 8
#endif

// All area lights
StructuredBuffer<AreaLight> LightBuffer;
RWStructuredBuffer<PackedPrecomputedLight> RWPrecomputedActiveLightBuffer;
StructuredBuffer<PackedPrecomputedLight> PrecomputedActiveLightBuffer;

RWStructuredBuffer<uint> RWActiveLightListCount;
RWStructuredBuffer<uint> RWActiveLightListBuffer;
StructuredBuffer<uint> ActiveLightListCount;
StructuredBuffer<uint> ActiveLightListBuffer;


StructuredBuffer<uint> LightGrid_ListLightIndexBuffer;
StructuredBuffer<uint> LightGrid_GridLightListOffsetBuffer;
StructuredBuffer<float> LightGrid_GridLightListCdfBuffer;
StructuredBuffer<uint> LightGrid_GridLightListLengthBuffer;
// Record the combination of light encodings that successfully illuminated geometries in the grid
StructuredBuffer<uint4> LightGrid_BloomFilterBuffer;

RWStructuredBuffer<uint> RWLightGrid_ListAllocatorBuffer;

RWStructuredBuffer<uint> RWLightGrid_ListLightIndexBuffer;
RWStructuredBuffer<uint> RWLightGrid_GridLightListCdfBuffer;
RWStructuredBuffer<uint> RWLightGrid_GridLightListOffsetBuffer;
RWStructuredBuffer<uint> RWLightGrid_GridLightListLengthBuffer;
RWStructuredBuffer<uint> RWLightGrid_BloomFilterBuffer;

#ifndef TILE_SIZE
// Defaults to a smaller tile size for better thread coherency
#define TILE_SIZE 8
#endif

#ifndef THREAD_GROUP_SIZE
#define THREAD_GROUP_SIZE 128
#endif

#ifndef WAVE_SIZE
#define WAVE_SIZE 32
#endif

#define MAX_NUM_LIGHT_CASCADES 8
#if LIGHT_GRID_NUM_CASCADES > MAX_NUM_LIGHT_CASCADES
#error "LIGHT_GRID_NUM_CASCADES must be less than or equal to MAX_NUM_LIGHT_CASCADES"
#endif

struct LightStructureUB {
    uint3 LightGridSize;
    float LightGridCellSize;
    float3 LightGridCenter;
    uint LighGridNumCascadesUsed;
    uint LightGridMaxNumGridLights;
    uint LightGridNumCascadeGrids;
    uint LightGridNumGrids;
    float LightInjectionIntensityThreshold;
    float4 LightGridCascadeMin[LIGHT_GRID_NUM_CASCADES];
    float4 LightGridCascadeMax[LIGHT_GRID_NUM_CASCADES];
    uint FrameIndex;
    uint MaxNumLights;
    uint2 Unused;
};

ConstantBuffer<LightStructureUB> LightStructure_UB;

struct DirectLightingUB {
    uint FrameIndex;
    float ShadowRayTMax;
    float ShadowRayLengthMultiplier;
    uint Unused;
};

ConstantBuffer<DirectLightingUB> DirectLighting_UB;

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ClearLightGrid (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID == 0) {
        RWLightGrid_ListAllocatorBuffer[0] = 0;
        RWActiveLightListCount[0] = 0;
    }
    uint Index = DispatchID;
    if (Index >= LightStructure_UB.LighGridNumCascadesUsed * LightStructure_UB.LightGridNumGrids) {
        return;
    }
    RWLightGrid_GridLightListLengthBuffer[Index] = 0;
}

// returns the wold grid min
float3 LightGrid_GetGridBounds(int4 GridIndex, out float GridSize) {
    GridSize = LightStructure_UB.LightGridCellSize * pow(2, GridIndex.w);
    return LightStructure_UB.LightGridCascadeMin[GridIndex.w].xyz 
         + GridSize * GridIndex.xyz;
}

bool LightGrid_IsInsideGrid(int4 GridIndex, float3 Position) {
    float GridSize;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    return all(Position >= GridMin) && all(Position < GridMin + GridSize);
}

bool LightGrid_IsInsideAnyCascade(float3 Position) {
    // Check if the position is inside the largest cascade of grids
    return all(Position >= LightStructure_UB.LightGridCascadeMin[LIGHT_GRID_NUM_CASCADES - 1].xyz) 
        && all(Position < LightStructure_UB.LightGridCascadeMax[LIGHT_GRID_NUM_CASCADES - 1].xyz);
}

uint4 LightGrid_GetGridIndex(float3 Position) {
    float GridSize = LightStructure_UB.LightGridCellSize;

    [unroll(LIGHT_GRID_NUM_CASCADES)]
    for (uint i = 0; i < LightStructure_UB.LighGridNumCascadesUsed; i++) {
        if (all(Position >= LightStructure_UB.LightGridCascadeMin[i].xyz) 
         && all(Position < LightStructure_UB.LightGridCascadeMax[i].xyz)) {
            return uint4(
                uint3((Position - LightStructure_UB.LightGridCascadeMin[i].xyz) / GridSize),
                i
            );
        }
        GridSize = GridSize * 2;
    }
    return INVALID_UINT.xxxx;
}

uint LightGrid_GetGridIndex1 (uint4 GridIndex) {
    return GridIndex.x + GridIndex.y * LightStructure_UB.LightGridSize.x + GridIndex.z * LightStructure_UB.LightGridSize.x * LightStructure_UB.LightGridSize.y + GridIndex.w * LightStructure_UB.LightGridNumCascadeGrids;
}

uint4 LightGrid_GetGridIndex(uint GridIndex1) {
    uint4 GridIndex = uint4(
        GridIndex1 % LightStructure_UB.LightGridSize.x,
        GridIndex1 / LightStructure_UB.LightGridNumGrids % LightStructure_UB.LightGridSize.y,
        GridIndex1 / (LightStructure_UB.LightGridSize.x * LightStructure_UB.LightGridSize.y) % LightStructure_UB.LightGridSize.z,
        GridIndex1 / LightStructure_UB.LightGridNumCascadeGrids
    );
    return GridIndex;
}

// Precompute lights, gather light data for later injection
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void PrecomputeLights(uint DispatchID: SV_DispatchThreadID) {
    uint LightIndex = DispatchID;
    if (LightIndex >= LightStructure_UB.MaxNumLights) return;
    AreaLight LightData = LightBuffer[LightIndex];
    if (LightData.Flags == 0) return; // Invalid light, skip
    // Extract light data
    EvaluatedLight Evaluated = EvaluateLight(LightData);
    // Evaluate other features
    float3 N = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    // Precompute
    {
        PrecomputedLight L = (PrecomputedLight)0;
        L.V0 = Evaluated.V0;
        L.V1 = Evaluated.V1;
        L.V2 = Evaluated.V2;
        L.Normal = N;
        float Area = length(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0)) * 0.5f;
        L.Intensity = RadianceToLuminance(Evaluated.EstimatedAverageEmission) * Area;
        if (L.Intensity > 1e-5f) {
            // Allocate active light list
            uint WaveNumActiveLights = WaveActiveCountBits(true);
            uint WaveLightListOffset = 0;
            if (WaveIsFirstLane()) {
                InterlockedAdd(RWActiveLightListCount[0], WaveNumActiveLights, WaveLightListOffset);
            }
            WaveLightListOffset = WaveReadLaneFirst(WaveLightListOffset);
            uint WaveLightListIndex = WavePrefixCountBits(true);
            uint LightListIndex = WaveLightListOffset + WaveLightListIndex;
            // Precompute and store active lights
            RWActiveLightListBuffer[LightListIndex] = LightIndex;
            RWPrecomputedActiveLightBuffer[LightListIndex] = PackPrecomputedLight(L);
        }
    }
}

// A coarse estimtion used for light grid injection
float EstimateLightGridContribution(PrecomputedLight L, float3 GridMin, float GridSize) {
    // Estimate the contribution from the area light using appriximated solid angle
    // Here, L.Intensity is the luminance of the light x the area of the light

    // Calculate the distance from the light to the grid
    float3 GridCenter = GridMin + GridSize * 0.5f;
    float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3;
    float3 UnnormalizedDirection = GridCenter - LightCenter;
    float3 Direction = normalize(UnnormalizedDirection);
    float DotProduct = dot(Direction, L.Normal);
    if(DotProduct < 0) {
        // Offset the light position according to the light normal for conservative estimation
        DotProduct += GridSize * sqrt(3.f) + 0.01f;
    }
    float Distance = length(UnnormalizedDirection);

    // Assume that the light is small enough compared to the grid, estimate the solid angle.
    float CosineFactor = saturate(DotProduct);
    float SolidAngle = CosineFactor / max(Distance * Distance, 1e-6f);

    // Area is premultiplied to L.Intensity
    return L.Intensity * SolidAngle;
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
    uint NumActiveLights = ActiveLightListCount[0];
    bool bOverflow = false;
    uint NumGridLights = 0, WriteLocation = 0;
    uint SampledOffset = 0, CandidateOffset = MAX_NUM_GRID_LIGHTS;
    Random R = MakeRandom(17419142u + DispatchID, LightStructure_UB.FrameIndex);
    float U = R.rand();
    float SumWeights = 0.0f, SumSampledWeights = 0.f, SumCandidateWeights = 0.f;
    for (uint LightListIndex = 0; LightListIndex < NumActiveLights; LightListIndex++) {
        PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[LightListIndex]);
        float Weight = EstimateLightGridContribution(L, GridMin, GridSize);
        if (Weight > LightStructure_UB.LightInjectionIntensityThreshold) {
            // Avoid bank conflicts
            SharedGridLightListIndices[WriteLocation * WAVE_SIZE + LocalID] = LightListIndex;
            if (SampledOffset <= WriteLocation && WriteLocation < SampledOffset + MAX_NUM_GRID_LIGHTS)
                SumSampledWeights += Weight;
            if (CandidateOffset <= WriteLocation && WriteLocation < CandidateOffset + MAX_NUM_GRID_LIGHTS)
                SumCandidateWeights += Weight;
            SumWeights += Weight;
            WriteLocation++, NumGridLights++;
            if (WriteLocation == CandidateOffset + MAX_NUM_GRID_LIGHTS) {
                // Overflow, time to select which group to keep
                float Sum = SumSampledWeights + SumCandidateWeights;
                float P = SumCandidateWeights / max(Sum, 1e-6f);
                if (U < P) { // Replace the group with the candidate group
                    uint Temp = SampledOffset;
                    SampledOffset = CandidateOffset;
                    CandidateOffset = Temp;
                    SumSampledWeights = SumCandidateWeights;
                    U = U / P;
                } else {
                    U = (U - P) / (1.00001f - P);
                }
                // Reset candidate statistics
                SumCandidateWeights = 0;
                WriteLocation = SampledOffset;
            }
        }
    }
    uint NumSampledLights = min(NumGridLights, MAX_NUM_GRID_LIGHTS);
    if (WriteLocation != CandidateOffset) {
        // Final swapping
        float Sum = SumSampledWeights + SumCandidateWeights;
        float P = SumCandidateWeights / (Sum + 1e-6f);
        if (U < P) { // Replace the group with the candidate group
            uint Temp = SampledOffset;
            SampledOffset = CandidateOffset;
            CandidateOffset = Temp;
            SumSampledWeights = SumCandidateWeights;
            // Also, replace the light count with the number of candidate lights
            NumSampledLights = WriteLocation - CandidateOffset;
        }
    }
    // Write to grid
    RWLightGrid_GridLightListLengthBuffer[GridIndex1] = NumSampledLights;
    RWLightGrid_GridLightListCdfBuffer[GridIndex1] = SumSampledWeights / max(SumWeights, 1e-6f);
    if (LocalID == 0) SharedListElementsRequired = 0;
    GroupMemoryBarrierWithGroupSync();
    uint LocalOffset = 0;
    InterlockedAdd(SharedListElementsRequired, NumSampledLights, LocalOffset);
    GroupMemoryBarrierWithGroupSync();
    if (LocalID == 0) {
        InterlockedAdd(RWLightGrid_ListAllocatorBuffer[0], SharedListElementsRequired, SharedListOffsetBase);
    }
    GroupMemoryBarrierWithGroupSync();
    uint GlobalOffset = SharedListOffsetBase + LocalOffset;
    RWLightGrid_GridLightListOffsetBuffer[GridIndex1] = GlobalOffset;
    for (uint i = 0; i < NumSampledLights; i++) {
        uint LightListIndex = SharedGridLightListIndices[(SampledOffset + i) * WAVE_SIZE + LocalID];
        // This time we store active light list index in the grid buffer 
        RWLightGrid_ListLightIndexBuffer[GlobalOffset + i] = LightListIndex;
    }
}

struct LightSampler {
    float SumWeight;
    float SampleU[NUM_LIGHT_SAMPELR_SAMPLES];
    float Weights[NUM_LIGHT_SAMPELR_SAMPLES];
    uint ActiveLightListIndex[NUM_LIGHT_SAMPELR_SAMPLES];
};

LightSampler InitLightSampler(Random R) {
    LightSampler LS = (LightSampler)0;
    // Scatter samples
    for (int i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        float Step = (1.f / NUM_LIGHT_SAMPELR_SAMPLES);
        LS.SampleU[i] = saturateDown((R.rand() + i) * Step);
        LS.ActiveLightListIndex[i] = INVALID_UINT;
    }
    return LS;
}

void AddLightToSampler(inout LightSampler LS, float Weight, uint Index) {
    float U = Weight / (LS.SumWeight + Weight + 1e-6f);
    LS.SumWeight += Weight;
    for (uint i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        bool bSelect = false;
        if (LS.ActiveLightListIndex[i] == INVALID_UINT || U <= LS.SampleU[i]) bSelect = true;
        if (bSelect) {
            LS.ActiveLightListIndex[i] = Index;
            LS.SampleU[i] = saturateDown((LS.SampleU[i] - U) / (1.f - U));
            LS.Weights[i] = Weight;
        } else {
            LS.SampleU[i] = LS.SampleU[i] / U;
        }
    }
}

struct LightSample {
    uint Index;
    float3 Position;
    float Pdf;
    float3 Radiance;
    bool IsValid() {
        return Pdf > 0;
    }
};

float3 SampleAreaLightArea(float3 V0, float3 V1, float3 V2, float2 u, out float AreaPdf) {
    if (u.x + u.y > 1.f) {
        u = 1.f - u;
    }
    float3 Position = InterpolateBarycentrics(V0, V1, V2, u);
    AreaPdf = 2.f / length(cross(V1 - V0, V2 - V0));
    return Position;
}

LightSample SampleLightDiffuseWithCosineWeight(float3 Position, float3 Normal, EvaluatedLight Evaluated, float2 u2) {
    LightSample Result = (LightSample)0;
    Result.Position = SampleAreaLightArea(Evaluated.V0, Evaluated.V1, Evaluated.V2, u2, Result.Pdf);
    float2 UV = InterpolateBarycentrics(Evaluated.UV0, Evaluated.UV1, Evaluated.UV2, u2);
    // Convert area pdf to solid angle pdf
    float Distance = length(Result.Position - Position);
    float3 Direction = normalize(Result.Position - Position);
    float3 LightNormal = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    float Cosine = dot(LightNormal, -Direction);
    float ReceiverCosine = dot(Direction, Normal);
    Result.Pdf *= Distance * Distance / saturate(Cosine);
    float3 EvaluatedEmission = Evaluated.Emission;
    if (IsValid(Evaluated.EmissionTextureIndex))
        EvaluatedEmission += GetBindlessSRV(Evaluated.EmissionTextureIndex).SampleLevel(LinearWrapSampler, UV, 0).rgb;
    Result.Radiance = EvaluatedEmission * saturate(Cosine) * saturate(ReceiverCosine);
    return Result;
}

Texture2D<float> G_DepthTexture;
Texture2D<float4> G_NormalTexture;

// Direction (Normal uint packed), Length (float)
RWTexture2D<uint> RWDirectLightingRayIndexTexture; // Specify the shadow ray index in the following hybrid tracing process
RWTexture2D<float4> RWDirectLightingRadianceEstimateTexture; // Output buffer for direct lighting radiance estimates

Texture2D<uint2> DirectLightingSampleTexture;
Texture2D<float> G_HiZBuffer;
Texture2D<float> G_HistoryDepth;


// Dispatch a thread for each grid
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void SpawnLightSamples(uint2 GroupID: SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelIndex = GroupID * TILE_SIZE + LocalID;
    if (any(PixelIndex >= View.Camera.FilmDimensions)) return;

    float2 PixelUV = (PixelIndex + 0.5f) / float2(View.Camera.FilmDimensions);
    float ReversedZDepth = G_DepthTexture.SampleLevel(PointWrapSampler, PixelUV, 0);
    if (ReversedZDepth == 0) {
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return; // Skip empty pixels
    }

    float LinearDepth = ReversedZDepthToLinearDepth(GetActiveCamera(), ReversedZDepth);
    float3 WorldPosition = RecoverWorldPositionPixelCoords(GetActiveCamera(), PixelIndex, LinearDepth);
    float3 WorldNormal = normalize(G_NormalTexture.Load(uint3(PixelIndex, 0)).xyz - 0.5f.xxx);
    uint4 GridIndex = LightGrid_GetGridIndex(WorldPosition);
    uint GridIndex1 = LightGrid_GetGridIndex1(GridIndex);

    Random R = MakeRandom(PixelIndex.x + PixelIndex.y * 5839, LightStructure_UB.FrameIndex);
    LightSampler LS = InitLightSampler(R);

    bool bUniformGrid = WaveActiveAllEqual(GridIndex1);
    float GridSize = 0;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    uint NumGridLights = LightGrid_GridLightListLengthBuffer[GridIndex1];
    uint GridLightListOffset = LightGrid_GridLightListOffsetBuffer[GridIndex1];
    float ListCdf = LightGrid_GridLightListCdfBuffer[GridIndex1];
    
    if (bUniformGrid) {
        for (uint LightListIndex = 0; LightListIndex < NumGridLights; LightListIndex++) {
            uint ActiveLightListIndex = LightGrid_ListLightIndexBuffer[GridLightListOffset + LightListIndex];
            PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[ActiveLightListIndex]);
            float Weight = EstimateLightGridContribution(L, GridMin, GridSize);
            AddLightToSampler(LS, Weight, ActiveLightListIndex);
        }
    } else {
        // Process the light with the minimum index in the grid
        uint LightListIndex = 0, Iteration = 0;
        // TODO remove Iteration (used to prevent driver timeouts)
        // TODO add a noisy occlusion modifier to the target distribution
        while(LightListIndex < NumGridLights && Iteration < 256) {
            uint ActiveLightListIndex = INVALID_UINT;
            if (LightListIndex < NumGridLights) ActiveLightListIndex = LightGrid_ListLightIndexBuffer[GridLightListOffset + LightListIndex];
            uint WaveMinLightIndex = WaveActiveMin(ActiveLightListIndex);
            if (WaveMinLightIndex == ActiveLightListIndex) {
                PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[ActiveLightListIndex]);
                float Weight = EstimateLightGridContribution(L, GridMin, GridSize);
                AddLightToSampler(LS, Weight, ActiveLightListIndex);
                LightListIndex++;
            }
            Iteration++;
        }
    }
    float SumResampleWeights = 0.f;
    float U = R.rand();
    LightSample ReservedSample = (LightSample)0;
    // Spawn 1 sample for each light, and resample from the samples
    for (int SamplerLightListIndex = 0; SamplerLightListIndex < NUM_LIGHT_SAMPELR_SAMPLES; SamplerLightListIndex++) {
        uint ActiveLightListIndex = LS.ActiveLightListIndex[SamplerLightListIndex];
        uint LightIndex = ActiveLightListBuffer[ActiveLightListIndex];
        EvaluatedLight Evaluated = EvaluateLight(LightBuffer[LightIndex]);
        float2 u2 = R.rand2();
        LightSample Sample = SampleLightDiffuseWithCosineWeight(WorldPosition, WorldNormal, Evaluated, u2);
        // Clip samples with low pdf (potential fireflies)
        if (Sample.IsValid() && Sample.Pdf > 0.01f) {
            float CandidateWeight = LS.Weights[SamplerLightListIndex];;
            // Perception based weight (log (x + c))
            float TargetSampleWeight = dot(Sample.Radiance / Sample.Pdf, 1.f.xxx);
            // RIS
            float ResampleWeight = TargetSampleWeight / max(CandidateWeight, 1e-7f);
            if (ResampleWeight > 1e-4f) {
                float NewSumResampleWeights = SumResampleWeights + ResampleWeight;
                float CurrentLightU = ResampleWeight / NewSumResampleWeights;
                if (CurrentLightU > U) {
                    U /= CurrentLightU;
                    ReservedSample = Sample;
                } else {
                    U = (U - CurrentLightU) / max(1 - CurrentLightU, 1e-7f);
                }
                SumResampleWeights = NewSumResampleWeights;
            }
        }
    }
    if (ReservedSample.IsValid() && dot(ReservedSample.Radiance, 1.f.xxx) > 0) {
        // Final sample acquired, prepare visibility trace
        float3 RadianceEstimation = ListCdf * ReservedSample.Radiance / ReservedSample.Pdf;
        float3 TraceDirection = ReservedSample.Position - WorldPosition;
        float TraceDistance = length(TraceDirection);
        TraceDirection /= TraceDistance;
        // Write to the direct lighting sample buffer
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = float4(RadianceEstimation, 1.f);
        // Allocate rays for tracing
        bool bPrimaryThread = WaveIsFirstLane();
        uint WaveRayCount = WaveActiveCountBits(true);
        uint WaveRayOffset = 0;
        if (bPrimaryThread) {
            InterlockedAdd(RWRayToTraceCount[0], WaveRayCount, WaveRayOffset);
        }
        WaveRayOffset = WaveReadLaneFirst(WaveRayOffset);
        uint WaveLocalRayOffset = WavePrefixCountBits(true) + 1;
        uint RayIndex = WaveRayOffset + WaveLocalRayOffset;
        // Write ray trace data
        RWRayToTraceDirectionBuffer[RayIndex] = PackNormal(TraceDirection);
        RWRayToTraceOriginScreenCoordBuffer[RayIndex] = PackUint2x16(PixelIndex);
        RWRayToTraceStateBuffer[RayIndex] = PackRayToTraceState(0, false);
        
        RWDirectLightingRayIndexTexture[PixelIndex] = RayIndex;
    }
    else {
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
    }
}

StructuredBuffer<uint> RayToTraceCount;

// Trace rays with SSRT
[numthreads(WAVE_SIZE, 1, 1)]
void ScreenSpaceTraceForDirectLighting(uint DispatchThreadID: SV_DispatchThreadID) {
    uint RayIndex = DispatchThreadID;
    uint RayListIndex = RayIndex; // Before compaction, RayIndex == RayListIndex
    if(RayListIndex >= RayToTraceCount[0]) {
        return; // No rays to trace
    }
    RayToTrace RayToTrace = FetchRayToTraceWithScreenOrigin(RayIndex, DirectLighting_UB.ShadowRayTMax);

    uint2 PixelIndex = RayToTrace.OriginScreenCoord;
    float ReversedZDepth = G_DepthTexture.Load(uint3(PixelIndex, 0));
    // Screen space ray trace
    float3 Estimate = RWDirectLightingRadianceEstimateTexture[PixelIndex].rgb;
    CameraParameters C = GetActiveCamera();
    // Shadow ray trace
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldPosition = RecoverWorldPositionPixelCoords(GetActiveCamera(), PixelIndex, LinearDepth);
    float3 OffsetedWorldPosition = WorldPosition;
    {
        // Offset the origin a bit, but do not step outside the pixel. (25%)
        float3 Normal = normalize(G_NormalTexture.Load(uint3(PixelIndex, 0)).xyz - 0.5f.xxx);
        float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
        float MaxOffsetLength = LinearDepth * 1e-4f;
        float2 PixelSize = GetPixelWorldSize(C, LinearDepth);
        float ProjectionX = abs(dot(C.NormalizedRight, Normal));
        float ProjectionY = abs(dot(C.NormalizedUp, Normal));
        float Fraction = 0.25f;
        float MaxX = Fraction * PixelSize.x / max(ProjectionX, 1e-4f);
        float MaxY = Fraction * PixelSize.y / max(ProjectionY, 1e-4f);
        float OffsetLength = min(MaxOffsetLength, min(MaxX, MaxY));
        WorldPosition += OffsetLength * Normal;
    }

    uint2 PackedDirectLightingSample = DirectLightingSampleTexture.Load(uint3(PixelIndex, 0));
    float3 TraceDirection = UnpackNormal(PackedDirectLightingSample.x);
    float TraceTMax = asfloat(PackedDirectLightingSample.y) * DirectLighting_UB.ShadowRayLengthMultiplier;
    bool bHit = false;
    float3 HitUVZ = 0, LastVisibleUVZ = 0;
    float HitTileZ = 0;
    ScreenSpaceRayTrace(
        C, G_DepthTexture, G_HiZBuffer,
        WorldPosition, TraceDirection, TraceTMax,
        20, HybridTracing_UB.SSRT_RelativeTexelThickness, 0,
        bHit, HitUVZ, LastVisibleUVZ, HitTileZ
    );

    float3 HitWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(HitUVZ.xy), ZDepthToLinearDepth(C, HitUVZ.z));
    float HitDistance = min(length(HitWorldPosition - WorldPosition), TraceTMax);

    if (bHit) {
        // Double checking using history buffer
        float3 PreviousUVZ = ReprojectToPreviousUVZFromUVZ(C, HitUVZ);
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float Noise = InterleavedGradientNoise(UV, DirectLighting_UB.FrameIndex);
        if (all(PreviousUVZ.xy >= 0) && all(PreviousUVZ.xy < 1)) {
            // Calculate the expected depth of the pixel last frame
            float PrevZDepth = PreviousUVZ.z;

            // Lookup the actual depth at the same screen position last frame
            float HistoryZDepth = G_HistoryDepth.SampleLevel(PointClampSampler, PreviousUVZ.xy, 0).x;

            bHit = abs(HistoryZDepth - PrevZDepth) < HybridTracing_UB.SSRT_RelativeTexelThickness * 0.5f * lerp(.5f, 2.0f, Noise);
        }
    }

    if (bHit) {
        // Occluded, mark and exit
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = float4(Estimate.rgb, -1);
        return; 
    }
    // Not occluded, prepare for world trace.

    // If not hit, backward the ray a little from the last visible position for ray continuation
    if (!bHit) {
        float LinearDepth = ZDepthToLinearDepth(C, LastVisibleUVZ.z);
        float3 LastVisibleWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(LastVisibleUVZ.xy), LinearDepth);
        HitDistance = min(length(LastVisibleWorldPosition - WorldPosition), TraceTMax);
        float Bias = min(LinearDepth * HybridTracing_UB.RayContinuationBackwardBiasFactor, HitDistance * 0.5f);
        HitDistance = max(HitDistance - Bias, 0);

        // Allocate new rays for continuation
        uint WaveSurvivingRayCount = WaveActiveCountBits(true);
        uint WaveNextRayListOffset = 0;
        if (WaveIsFirstLane()) {
            InterlockedAdd(RWRayToTraceListAllocator[0], WaveSurvivingRayCount, WaveNextRayListOffset);
        }
        WaveNextRayListOffset = WaveReadLaneFirst(WaveNextRayListOffset);
        uint WaveLocalRayListOffset = WavePrefixCountBits(true);
        uint RayListIndex = WaveNextRayListOffset + WaveLocalRayListOffset;
        RWRayToTraceListBuffer[RayListIndex] = RayIndex;
    }
    // Remember to reduce the texel relative thickness on SSRT if there're too many false hits.

    // Write back ray data
    uint RayToTraceState = PackRayToTraceState(HitDistance, bHit);
    RWRayToTraceStateBuffer[RayIndex] = RayToTraceState;
}

// HWRT...

Texture2D<float4> DirectLightingRadianceEstimateTexture;
RWTexture2D<float4> RWDiffuseDirectLightingTexture;

// Render diffuse direct lighting using trace results
// 1 thread per ray
[numthreads(WAVE_SIZE, 1, 1)]
void RenderDiffuseDirectLighting(uint DispatchThreadID : SV_DispatchThreadID)
{ 
    uint RayIndex = DispatchThreadID;
    if(RayIndex >= RWRayToTraceCount[0]) return;
    RayToTrace RayToTrace = FetchRayToTraceWithScreenOrigin(RayIndex, 0);
    if (!RayToTrace.bHit) {
        CameraParameters C = GetActiveCamera();
        uint2 PixelIndex = RayToTrace.OriginScreenCoord;
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float3 Estimate = DirectLightingRadianceEstimateTexture.SampleLevel(PointClampSampler, UV, 0).rgb;
        RWDiffuseDirectLightingTexture[PixelIndex] = float4(Estimate, 1.f);
    }
}
