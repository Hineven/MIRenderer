#include "shared/SharedView.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedLight.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/BindlessTextures.hlsl"
#include "shared/SharedStaticMesh.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Math.hlsl"
#include "headers/Radiometry.hlsl"
#include "headers/Random.hlsl"
#include "headers/GeometryBuffers.hlsl"

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

// Precompute lights, making it easier to estiamte their contributions
struct PrecomputedLight {
    float3 V0, V1, V2; // Triangle vertices
    float3 Normal;     // Triangle normal
    float Intensity;
    bool bInvalid;
};

struct PackedPrecomputedLight {
    float3 V0, V1, V2; // Triangle vertices
    uint Normal;       // Triangle normal
    float Intensity;
};

// All area lights
StructuredBuffer<Light> Lights;
StructuredBuffer<uint> LightCount;
RWStructuredBuffer<PackedPrecomputedLight> RWPrecomputedLights;
StructuredBuffer<PackedPrecomputedLight> PrecomputedLights;

StructuredBuffer<uint> LightGrid_ListIndex;
StructuredBuffer<uint> LightGrid_ListCdf;
StructuredBuffer<uint> LightGrid_GridLightListOffsets;
StructuredBuffer<float> LightGrid_GridLightListCdf;
StructuredBuffer<uint> LightGrid_GridLightListLengths;
// Record the combination of light encodings that successfully illuminated geometries in the grid
StructuredBuffer<uint2> LightGrid_Bloom;

RWStructuredBuffer<uint> RWLightGrid_ListAllocator;

RWStructuredBuffer<uint> RWLightGrid_ListLightIndex;
RWStructuredBuffer<uint> RWLightGrid_GridLightListCdf;
RWStructuredBuffer<uint> RWLightGrid_GridLightListOffsets;
RWStructuredBuffer<uint> RWLightGrid_GridLightListLengths;
RWStructuredBuffer<uint> RWLightGrid_Bloom;

StructuredBuffer<RenderableHeader> RenderableHeaders;
StructuredBuffer<float3x4>         RenderableTransforms;
StructuredBuffer<float3x3>         RenderableNormalTransforms; // InvTranspose of RenderableTransforms
StructuredBuffer<uint2>            RenderableIndexAndMaterialIndex;
StructuredBuffer<MaterialHeader>   MaterialHeaders;

StructuredBuffer<StaticMeshHeader> StaticMeshHeaderBuffer;
StructuredBuffer<GeometryHeader> GeometryHeaderBuffer;
StructuredBuffer<uint2> StaticMeshDescriptionBuffer;
StructuredBuffer<DefaultStaticMeshVertex> VertexBuffer;
StructuredBuffer<uint> IndexBuffer;
StructuredBuffer<MaterialHeader> MaterialHeaderBuffer;

SamplerState Sampler;

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
    float4 LightGridCascadeMin[MAX_NUM_LIGHT_CASCADES];
    float4 LightGridCascadeMax[MAX_NUM_LIGHT_CASCADES];
};

ConstantBuffer<LightStructureUB> LightStructure_UB;

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ClearLightGrid (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID == 0) RWLightGrid_ListAllocator[0] = 0;
    uint Index = DispatchID;
    if (Index >= LightStructure_UB.LighGridNumCascadesUsed * LightStructure_UB.LightGridNumGrids) {
        return;
    }
    RWLightGrid_GridLightListLengths[Index] = 0;
}

PackedPrecomputedLight PackPrecomputedLight(PrecomputedLight L) {
    PackedPrecomputedLight P = (PackedPrecomputedLight)0;
    P.V0 = L.V0;
    P.V1 = L.V1;
    P.V2 = L.V2;
    P.Normal = PackNormal(L.Normal);
    P.Intensity = L.Intensity;
    return P;
}

PrecomputedLight UnpackPrecomputedLight(PackedPrecomputedLight P) {
    PrecomputedLight L = (PrecomputedLight)0;
    L.V0 = P.V0;
    L.V1 = P.V1;
    L.V2 = P.V2;
    L.Normal = UnpackNormal(P.Normal); 
    L.Intensity = P.Intensity;
    return L;
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
        GridIndex1 / LightStructure_UB.LightGridNumCascadeGrids,
    );
    return GridIndex;
}

struct EvaluatedLight {
    float3 V0, V1, V2;
    float2 UV0, UV1, UV2;
    uint EmissionTextureIndex;
    float3 Emission;
    float3 EstimatedAverageEmission;
};

EvaluatedLight EvaluateLight(Light LightData) {
    EvaluatedLight EvaluatedLightData = (EvaluatedLight)0;
    // Extract light data
    uint RenderableIndex = LightData.Data0.x;
    uint StaticMeshDescriptionOffset = LightData.Data0.y;
    StaticMeshHeader StaticMesh = StaticMeshHeaderBuffer[RenderableIndex];
    uint DescriptionIndex = StaticMesh.DescriptionOffset + StaticMeshDescriptionOffset;
    uint2 GeometryMaterial = StaticMeshDescriptionBuffer[DescriptionIndex];
    uint PrimitiveIndex = LightData.Data0.z;
    uint LightFlags = LightData.Data0.w;
    // TODO monitor light changes
    float3x4 ToWorldTransform = RenderableTransforms[RenderableIndex];
    GeometryHeader Geometry = GeometryHeaderBuffer[GeometryMaterial.x];
    uint VertexOffset = Geometry.VertexOffset;
    uint IndexOffset = Geometry.IndexOffset + PrimitiveIndex * 3;
    uint I0 = VertexOffset + IndexBuffer[IndexOffset];
    uint I1 = VertexOffset + IndexBuffer[IndexOffset + 1];
    uint I2 = VertexOffset + IndexBuffer[IndexOffset + 2];
    EvaluatedLightData.V0 = TransformPoint(ToWorldTransform, VertexBuffer[I0].Position);
    EvaluatedLightData.V1 = TransformPoint(ToWorldTransform, VertexBuffer[I1].Position);
    EvaluatedLightData.V2 = TransformPoint(ToWorldTransform, VertexBuffer[I2].Position);
    // Estimate emission
    MaterialHeader Material = MaterialHeaders[GeometryMaterial.y];
    EvaluatedLightData.Emission = Material.Emissive;
    if (IsValid(Material.EmissiveMap)) {
        // Sample the center pixel for approximation
        // TODO: Sample a qualified LOD
        float2 UV0 = VertexBuffer[I0].UV;
        float2 UV1 = VertexBuffer[I1].UV;
        float2 UV2 = VertexBuffer[I2].UV;
        float2 UV = (UV0 + UV1 + UV2) * (1.f / 3.f);
        EvaluatedLightData.EstimatedAverageEmission = GetBindlessSRV(Material.EmissiveMap).SampleLevel(Sampler, UV, 0).rgb;
    }
    EvaluatedLightData.EmissionTextureIndex = Material.EmissiveMap;
    EvaluatedLightData.EstimatedAverageEmission += Material.Emissive;
    return EvaluatedLightData;
}

// Precompute lights, gather light data for later injection
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void PrecomputeLights(uint DispatchID: SV_DispatchThreadID) {
    uint LightIndex = DispatchID;
    if (LightIndex >= LightCount[0]) return;
    Light LightData = Lights[LightIndex];
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
        RWPrecomputedLights[LightIndex] = PackPrecomputedLight(L);
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
groupshared uint SharedGridLightIndices[WAVE_SIZE * MAX_NUM_GRID_LIGHTS * 2];
[numthreads(WAVE_SIZE, 1, 1)]
void InjectLights(uint DispatchID: SV_DispatchThreadID, uint LocalID : SV_GroupThreadID) {
    if (DispatchID >= LightStructure_UB.LightGridNumGrids) return;
    uint GridIndex1 = DispatchID;
    // x, y, z, cascade
    uint4 GridIndex = LightGrid_GetGridIndex(GridIndex1);
    float GridSize;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    // TODO use multi level injection for a large number of lights
    uint NumLights = LightCount[0];
    bool bOverflow = false;
    uint NumGridLights = 0, WriteLocation = 0;
    uint SampledOffset = 0, CandidateOffset = MAX_NUM_GRID_LIGHTS;
    Random R = MakeRandom(17419142u + DispatchID, LightStructure_UB.FrameIndex);
    float U = R.rand();
    float SumWeights = 0.0f, SumSampledWeights = 0.f, SumCandidateWeights = 0.f;
    for (uint LightIndex = 0; LightIndex < NumLights; LightIndex++) {
        PrecomputedLight L = UnpackPrecomputedLight(PrecomputedLights[LightIndex]);
        float Weight = EstimateLightGridContribution(L, GridMin, GridSize);
        if (Weight > LightStructure_UB.LightInjectionIntensityThreshold) {
            // Avoid bank conflicts
            SharedGridLightIndices[WriteLocation * WAVE_SIZE + LocalID] = LightIndex;
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
    RWLightGrid_GridLightListLengths[GridIndex1] = NumSampledLights;
    RWLightGrid_GridLightListCdf[GridIndex1] = SumSampledWeights / max(SumWeights, 1e-6f);
    if (LocalID == 0) SharedListElementsRequired = 0;
    GroupMemoryBarrierWithGroupSync();
    uint LocalOffset = 0;
    InterlockedAdd(SharedListElementsRequired, NumSampledLights, LocalOffset);
    GroupMemoryBarrierWithGroupSync();
    if (LocalID == 0) {
        InterlockedAdd(RWLightGrid_ListAllocator[0], SharedListElementsRequired, SharedListOffsetBase);
    }
    GroupMemoryBarrierWithGroupSync();
    uint GlobalOffset = SharedListOffsetBase + LocalOffset;
    RWLightGrid_GridLightListOffsets[GridIndex1] = GlobalOffset;
    for(uint i = 0; i < NumSampledLights; i++) {
        uint Light = SharedGridLightIndices[(SampledOffset + i) * WAVE_SIZE + LocalID];
        RWLightGrid_ListLightIndex[GlobalOffset + i] = Light;
    }
}

struct LightSampler {
    float SumWeight;
    float SampleU[NUM_LIGHT_SAMPELR_SAMPLES];
    float Weights[NUM_LIGHT_SAMPELR_SAMPLES];
    uint LightIndex[NUM_LIGHT_SAMPELR_SAMPLES];
}

LightSampler InitLightSampler(Random R) {
    LightSampler LS = (LightSampler)0;
    // Scatter samples
    for (int i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        float Step = (1.f / NUM_LIGHT_SAMPELR_SAMPLES);
        LS.SampleU[i] = saturateDown((R.rand() + i) * Step);
        LS.LightIndex[i] = INVALID_UINT;
    }
    return LS;
}

void AddLightToSampler(inout LightSampler LS, float Weight, uint Index) {
    float U = Weight / (LS.SumWeight + Weight + 1e-6f);
    LS.SumWeight += Weight;
    for (uint i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        bool bSelect = false;
        if (LS.LightIndex[i] == INVALID_UINT || U <= LS.SampleU[i]) bSelect = true;
        if (bSelect) {
            LS.LightIndex[i] = Index;
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
        EvaluatedEmission += GetBindlessSRV(Evaluated.EmissionTextureIndex).SampleLevel(Sampler, UV, 0).rgb; 
    Result.Radiance = EvaluatedEmission * saturate(Cosine) * saturate(ReceiverCosine);
    return Result;
}

Texture2D<float> G_DepthTexture;
Texture2D<float4> G_NormalTexture;

// Direction (Normal uint packed), Length (float)
RWTexture2D<uint2> G_DirectLightingSampleTexture; // Output buffer for direct lighting samples
RWTexture2D<float4> G_DirectLightingRadianceEstimateTexture; // Output buffer for direct lighting radiance estimates

// Dispatch a thread for each grid
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void SpawnLightSamples(uint2 GroupID: SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelIndex = GroupID * TILE_SIZE + LocalID;
    if (any(PixelIndex >= View.Camera.FilmDimensions)) return;

    float ReversedZDepth = G_DepthTexture.Load(uint3(PixelIndex, 0));
    float3 WorldPosition = RecoverWorldPosition(GetActiveCamera(), PixelIndex, ReversedZDepth);
    float3 WorldNormal = normalize(G_NormalTexture.Load(uint3(PixelIndex, 0)).xyz - 0.5f.xxx);
    uint4 GridIndex = LightGrid_GetGridIndex(WorldPosition);
    uint GridIndex1 = LightGrid_GetGridIndex1(GridIndex);

    Random R = MakeRandom(PixelIndex.x + PixelIndex.y * 5839, LightStructure_UB.FrameIndex);
    LightSampler LS = InitLightSampler(R);

    bool bUniformGrid = WaveActiveAllEqual(GridIndex1);
    float GridSize = 0;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    uint NumGridLights = LightGrid_GridLightListLengths[GridIndex1];
    uint GridLightListOffset = LightGrid_GridLightListOffsets[GridIndex1];
    float ListCdf = LightGrid_GridLightListCdf[GridIndex1];
    
    if (bUniformGrid) {
        for (uint LightListIndex = 0; LightListIndex < NumGridLights; LightListIndex++) {
            uint LightIndex = LightGrid_ListIndex[GridLightListOffset + LightListIndex];
            PrecomputedLight L = UnpackPrecomputedLight(PrecomputedLights[LightIndex]);
            float Weight = EstimateLightGridContribution(L, GridMin, GridSize);
            AddLightToSampler(LS, Weight, LightIndex);
        }
    } else {
        // Process the light with the minimum index in the grid
        uint LightListIndex = 0, Iteration = 0;
        // TODO remove Iteration (used to prevent driver timeouts)
        // TODO add a noisy occlusion modifier to the target distribution
        while(LightListIndex < NumGridLights && Iteration < 256) {
            uint LightIndex = INVALID_UINT;
            if (LightListIndex < NumGridLights) LightIndex = LightGrid_ListIndex[GridLightListOffset + LightListIndex];
            uint WaveMinLightIndex = WaveActiveMin(LightIndex);
            if (WaveMinLightIndex == LightIndex) {
                PrecomputedLight L = UnpackPrecomputedLight(PrecomputedLights[LightIndex]);
                float Weight = EstimateLightGridContribution(L, GridMin, GridSize);
                AddLightToSampler(LS, Weight, LightIndex);
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
        uint LightIndex = LS.LightIndex[SamplerLightListIndex];
        EvaluatedLight Evaluated = EvaluateLight(Lights[LightIndex]);
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
    if (ReservedSample.IsValid()) {
        // Final sample acquired, prepare visibility trace
        float3 RadianceEstimation = ListCdf * ReservedSample.Radiance / ReservedSample.Pdf;
        float3 TraceDirection = ReservedSample.Position - WorldPosition;
        float TraceDistance = length(TraceDirection);
        TraceDirection /= TraceDistance;
        // Write to the direct lighting sample buffer
        G_DirectLightingSampleTexture[PixelIndex] = uint2(PackNormal(TraceDirection), asuint(TraceDistance));
        G_DirectLightingRadianceEstimateTexture[PixelIndex] = float4(RadianceEstimation, 1.f);
    }
    else {
        G_DirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
    }
}