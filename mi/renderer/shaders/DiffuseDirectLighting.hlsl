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

RWStructuredBuffer<uint> RWRayToTraceCount;
RWStructuredBuffer<uint> RWVolumeRayToTraceCount;

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
RWStructuredBuffer<float> RWLightGrid_GridLightListCdfBuffer;
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

// Sometimes when involving ray compaction / continuation, allocate new rays on this buffer
RWStructuredBuffer<uint> RWRayToTraceListAllocator;
RWStructuredBuffer<uint> RWRayToTraceListBuffer;

RWStructuredBuffer<uint> RWRayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWRayToTraceStateBuffer;

// Optional (when the starting point is exactly on a pixel center)
RWStructuredBuffer<uint> RWRayToTraceOriginScreenCoordBuffer;

// Optional (when the ray origin is in world space)
RWStructuredBuffer<float3> RWRayToTraceOriginBuffer;

struct HybridTracingUB {
    float SSRT_RelativeTexelThickness;
    float RayContinuationBackwardBiasFactor;
    float DefaultTMax;
    uint Unused2;
};
ConstantBuffer<HybridTracingUB> HybridTracing_UB;

RayToTrace FetchRayToTraceWithWorldOrigin(uint RayIndex, float TMax) {
    RayToTrace Ray = (RayToTrace)0;
    Ray.Origin = RWRayToTraceOriginBuffer[RayIndex];
    Ray.Direction = UnpackNormal(RWRayToTraceDirectionBuffer[RayIndex]);
    uint RayToTraceState = RWRayToTraceStateBuffer[RayIndex];
    Ray.TMax = TMax;
    Ray.TCurrent = UnpackRayToTraceState(RayToTraceState, Ray.bHit);
    return Ray;
}

RayToTrace FetchRayToTraceWithScreenOrigin(uint RayIndex, float TMax) {
    RayToTrace Ray = (RayToTrace)0;
    Ray.OriginScreenCoord = UnpackUint2x16(RWRayToTraceOriginScreenCoordBuffer[RayIndex]);
    Ray.Direction = UnpackNormal(RWRayToTraceDirectionBuffer[RayIndex]);
    uint RayToTraceState = RWRayToTraceStateBuffer[RayIndex];
    Ray.TMax = TMax;
    Ray.TCurrent = UnpackRayToTraceState(RayToTraceState, Ray.bHit);
    return Ray;
}


[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void ClearLightGrid (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID == 0) {
        RWLightGrid_ListAllocatorBuffer[0] = 0;
        RWActiveLightListCount[0] = 0;
        RWRayToTraceCount[0] = 0;
        RWRayToTraceListAllocator[0] = 0;
        RWVolumeRayToTraceCount[0] = 0;
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
    return GridIndex.x + GridIndex.y * LightStructure_UB.LightGridSize.x 
    + GridIndex.z * LightStructure_UB.LightGridSize.x * LightStructure_UB.LightGridSize.y 
    + GridIndex.w * LightStructure_UB.LightGridNumCascadeGrids;
}

uint4 LightGrid_GetGridIndex(uint GridIndex1) {
    uint4 GridIndex = uint4(
        GridIndex1 % LightStructure_UB.LightGridSize.x,
        GridIndex1 / LightStructure_UB.LightGridSize.x % LightStructure_UB.LightGridSize.y,
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

// A coarse estimtion used for light -> point contribution
float EstimateLightContribution(PrecomputedLight L, float3 Position, float3 Normal, bool bVolume = false) {
    if(bVolume) {
        // Position is from a sample of volume scattering media. Normal is the view direction. 
        float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3.0f;
        float3 ToLightCenter = LightCenter - Position;
        float DistanceSq = dot(ToLightCenter, ToLightCenter);

        // The the light is not facing the sampled position. Cull it out.
        if (dot(ToLightCenter, L.Normal) >= 0.0f) {
            return 0.0f;
        }
        
        float LightArea = length(cross(L.V1 - L.V0, L.V2 - L.V0)) * 0.5f;
        float3 CenterToV0 = L.V0 - LightCenter;
        float3 CenterToV1 = L.V1 - LightCenter;
        float3 CenterToV2 = L.V2 - LightCenter;
        float3 LightVertexDistancesSq = float3(
            dot(CenterToV0, CenterToV0),
            dot(CenterToV1, CenterToV1),
            dot(CenterToV2, CenterToV2)
        );
        float MaxLightRadiusSq = max(LightVertexDistancesSq.x, max(LightVertexDistancesSq.y, LightVertexDistancesSq.z));
        
        float CosineBias = 0;
        {
            // The light is big & close enough (distance < 2 * max light radius), reduce the effect from CosineFactor
            CosineBias = 1.f - saturate(DistanceSq / (2 * MaxLightRadiusSq));
        }
        // Regarding the nature of volume scattering, lights that the sample is not facing towards will still have a lower
        // effect on the sample. So a constant bias of 1.5 and a scaling factor of 0.4 are applied.
        float CosineFactor = saturate(CosineBias + (1.5f + dot(-Normal, ToLightCenter)) * 0.4f);
        // TODO take account of different parameterizations of HG phase function

        float SolidAngle = LightArea / (DistanceSq + LightArea);
        return L.Intensity * SolidAngle * CosineFactor / PI;
    } else {
        float3 LightCenter = (L.V0 + L.V1 + L.V2) / 3.0f;
        float3 ToLightCenter = LightCenter - Position;
        float DistanceSq = dot(ToLightCenter, ToLightCenter);

        float3 ToV0 = L.V0 - Position;
        float3 ToV1 = L.V1 - Position;
        float3 ToV2 = L.V2 - Position;

        // The the light is not facing the sampled position. Cull it out.
        if (dot(ToLightCenter, L.Normal) >= 0.0f) {
            return 0.0f;
        }

        float k0 = saturate(dot(Normal, ToV0));
        float k1 = saturate(dot(Normal, ToV1));
        float k2 = saturate(dot(Normal, ToV2));
        // the sampled surface is not facing the light. Cull it out.
        if (all(float3(k0, k1, k2) <= 0.0f)) {
            return 0.0f;
        }
        
        float LightArea = length(cross(L.V1 - L.V0, L.V2 - L.V0)) * 0.5f;
        float3 CenterToV0 = L.V0 - LightCenter;
        float3 CenterToV1 = L.V1 - LightCenter;
        float3 CenterToV2 = L.V2 - LightCenter;
        float3 LightVertexDistancesSq = float3(
            dot(CenterToV0, CenterToV0),
            dot(CenterToV1, CenterToV1),
            dot(CenterToV2, CenterToV2)
        );
        float MaxLightRadiusSq = max(LightVertexDistancesSq.x, max(LightVertexDistancesSq.y, LightVertexDistancesSq.z));
        
        float CosineBias = 0;
        {
            // The light is big & close enough (distance < 2 * max light radius), reduce the effect from CosineFactor
            CosineBias = 1.f - saturate(DistanceSq / (2 * MaxLightRadiusSq));
        }

        float CosineFactor = saturate(CosineBias + dot(Normal, ToLightCenter));

        float SolidAngle = LightArea / (DistanceSq + LightArea);
        return L.Intensity * SolidAngle * CosineFactor / PI;
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
    uint NumActiveLights = ActiveLightListCount[0];
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
                float P = SumCandidateWeights / max(SumWeights, 1e-6f);
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
        float P = SumCandidateWeights / (SumWeights + 1e-6f);
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

LightSampler InitLightSampler(inout Random R) {
    LightSampler LS = (LightSampler)0;
    // Scatter samples
    for (int i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        float Step = (1.f / NUM_LIGHT_SAMPELR_SAMPLES);
        LS.SampleU[i] = saturateDown((R.rand() + i) * Step);
        LS.ActiveLightListIndex[i] = INVALID_UINT;
        LS.Weights[i] = 0.f;
    }
    return LS;
}

void AddLightToSampler(inout LightSampler LS, float Weight, uint Index) {
    float U = Weight / (LS.SumWeight + Weight + 1e-6f);
    LS.SumWeight += Weight;
    for (uint i = 0; i < NUM_LIGHT_SAMPELR_SAMPLES; i++) {
        bool bSelect = false;
        if (U > LS.SampleU[i]) bSelect = true;
        if (bSelect) {
            LS.ActiveLightListIndex[i] = Index;
            LS.SampleU[i] = LS.SampleU[i] / U;
            LS.Weights[i] = Weight;
        } else {
            LS.SampleU[i] = saturateDown((LS.SampleU[i] - U) / (1.f - U));
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

float3 SampleAreaLightArea(float3 V0, float3 V1, float3 V2, float2 u, out float Area, out float AreaPdf) {
    if (u.x + u.y > 1.f) {
        u = 1.f - u;
    }
    float3 Position = InterpolateBarycentrics(V0, V1, V2, u);
    Area = length(cross(V1 - V0, V2 - V0)) / 2.f;
    AreaPdf = 1.f / max(Area, 1e-5f);
    return Position;
}

LightSample SampleLightDiffuseWithPreMultipliedCosine(float3 Position, float3 Normal, EvaluatedLight Evaluated, float2 u2) {
    LightSample Result = (LightSample)0;
    float Area = 0.f;
    Result.Position = SampleAreaLightArea(Evaluated.V0, Evaluated.V1, Evaluated.V2, u2, Area, Result.Pdf);
    float2 UV = InterpolateBarycentrics(Evaluated.UV0, Evaluated.UV1, Evaluated.UV2, u2);
    // Convert area pdf to solid angle pdf
    float Distance = length(Result.Position - Position);
    float3 Direction = normalize(Result.Position - Position);
    float3 LightNormal = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    float Cosine = dot(LightNormal, -Direction);
    float ReceiverCosine = dot(Direction, Normal);
    Result.Pdf *= Distance * Distance / max(abs(Cosine), 1e-4f);
    float3 EvaluatedEmission = Evaluated.Emission;
    if (IsValid(Evaluated.EmissionTextureIndex))
        EvaluatedEmission += GetBindlessSRV(Evaluated.EmissionTextureIndex).SampleLevel(LinearWrapSampler, UV, 0).rgb;
    Result.Radiance = EvaluatedEmission * saturate(Cosine) * saturate(ReceiverCosine);
    return Result;
}

float HenyeyGreensteinPhaseFunction(float Cosine, float g) {
    // Henyey-Greenstein phase function
    float g2 = g * g;
    float denom_term = 1.f + g2 + 2.f * g * Cosine;
    return (1.f - g2) / (4.f * PI * denom_term * sqrt(denom_term));
}

// Samples a direction based on the Henyey-Greenstein phase function.
// g: anisotropy parameter, in [-1, 1].
// u: 2D uniform random sample.
// pdf: output spherical PDF for the sampled direction.
// Returns a sampled direction in a local coordinate system (around Z-axis).
float3 SampleHenyeyGreenstein(float g, float2 u, out float Pdf) {
    float CosTheta;
    // Handle the isotropic case (g=0) to avoid division by zero and for precision.
    if (abs(g) < 1e-4f) {
        CosTheta = 1.f - 2.f * u.x;
    } else {
        float g2 = g * g;
        float term = (1.f - g2) / (1.f - g + 2.f * g * u.x);
        CosTheta = (1.f / (2.f * g)) * (1.f + g2 - term * term);
    }

    // Convert spherical coordinates to a Cartesian direction vector.
    float SinTheta = sqrt(max(0.f, 1.f - CosTheta * CosTheta));
    float Phi = 2.f * PI * u.y;
    
    float3 sample_dir = float3(
        cos(Phi) * SinTheta,
        sin(Phi) * SinTheta,
        CosTheta
    );

    // The PDF of this sample is the HG function itself.
    Pdf = HenyeyGreensteinPhaseFunction(CosTheta, g);

    return sample_dir;
}

LightSample SampleLightWithPreMultipliedPhaseFunction(
    float3 Position, float3 ViewDirection, EvaluatedLight Evaluated, float g, float2 u2
) {
    LightSample Result = (LightSample)0;
    float Area = 0.f;
    Result.Position = SampleAreaLightArea(Evaluated.V0, Evaluated.V1, Evaluated.V2, u2, Area, Result.Pdf);
    float2 UV = InterpolateBarycentrics(Evaluated.UV0, Evaluated.UV1, Evaluated.UV2, u2);
    // Convert area pdf to solid angle pdf
    float Distance = length(Result.Position - Position);
    float3 Direction = normalize(Result.Position - Position);
    float3 LightNormal = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    float Cosine = dot(LightNormal, -Direction);
    float ReceiverCosine = dot(Direction, ViewDirection);
    Result.Pdf *= Distance * Distance / max(abs(Cosine), 1e-4f);
    float3 EvaluatedEmission = Evaluated.Emission;
    if (IsValid(Evaluated.EmissionTextureIndex))
        EvaluatedEmission += GetBindlessSRV(Evaluated.EmissionTextureIndex).SampleLevel(LinearWrapSampler, UV, 0).rgb;
    Result.Radiance = EvaluatedEmission * saturate(Cosine) * HenyeyGreensteinPhaseFunction(ReceiverCosine, g);
    return Result;
}

Texture2D<float> G_DepthTexture;
Texture2D<float4> G_NormalTexture;

RWStructuredBuffer<float> RWShadowRayToTraceTMaxBuffer;
StructuredBuffer<float> ShadowRayToTraceTMaxBuffer;


// Direction (Normal uint packed), Length (float)
RWTexture2D<uint> RWDirectLightingRayIndexTexture; // Specify the shadow ray index in the following hybrid tracing process
RWTexture2D<float4> RWDirectLightingRadianceEstimateTexture; // Output buffer for direct lighting radiance estimates

Texture2D<float> G_HiZBuffer;
Texture2D<float> G_HistoryDepth;

Texture2D<float4> DirectLightingRadianceEstimateTexture;
RWTexture2D<float4> RWDiffuseDirectLightingTexture;

// Dispatch a thread for each tile
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void SpawnLightSamples(uint2 GroupID: SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelIndex = GroupID * TILE_SIZE + LocalID;
    if (any(PixelIndex >= View.Camera.FilmDimensions)) return;

    CameraParameters C = GetActiveCamera();
    float2 PixelUV = ScreenCoordsToUV(C, PixelIndex);
    float ReversedZDepth = G_DepthTexture.SampleLevel(PointClampSampler, PixelUV, 0);
    if (ReversedZDepth == 0) {
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return; // Skip empty pixels
    }

    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
    float3 WorldNormal = normalize(G_NormalTexture.SampleLevel(PointClampSampler, PixelUV, 0).xyz - 0.5f.xxx);
    uint4 GridIndex = LightGrid_GetGridIndex(WorldPosition);
    if (!IsValid(GridIndex.x)) {
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return; // Out of light grid
    }
    uint GridIndex1 = LightGrid_GetGridIndex1(GridIndex);

    Random R = MakeRandom(32618420u + PixelIndex.x + PixelIndex.y * 5839, LightStructure_UB.FrameIndex);
    LightSampler LS = InitLightSampler(R);

    bool bUniformGrid = WaveActiveAllEqual(GridIndex1);
    float GridSize = 0;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    uint NumGridLights = LightGrid_GridLightListLengthBuffer[GridIndex1];
    uint GridLightListOffset = LightGrid_GridLightListOffsetBuffer[GridIndex1];
    float ListCdf = LightGrid_GridLightListCdfBuffer[GridIndex1];
    
    uint NumNonZeroGridLights = 0;
    if (bUniformGrid) {
        for (uint LightListIndex = 0; LightListIndex < NumGridLights; LightListIndex++) {
            uint ActiveLightListIndex = LightGrid_ListLightIndexBuffer[GridLightListOffset + LightListIndex];
            PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[ActiveLightListIndex]);
            float Weight = EstimateLightContribution(L, WorldPosition, WorldNormal);
            if(Weight > 0.f) {
                AddLightToSampler(LS, Weight, ActiveLightListIndex);
                NumNonZeroGridLights ++;
            }
        }
    } else {
        // Process the light with the minimum index in the grid
        uint LightListIndex = 0, Iteration = 0;
        // TODO remove Iteration (used to prevent driver timeouts)
        // TODO add a noisy occlusion modifier based on history cache to the target distribution
        while(LightListIndex < NumGridLights && Iteration < 256) {
            uint ActiveLightListIndex = INVALID_UINT;
            ActiveLightListIndex = LightGrid_ListLightIndexBuffer[GridLightListOffset + LightListIndex];
            uint WaveMinLightIndex = WaveActiveMin(ActiveLightListIndex);
            if (WaveMinLightIndex == ActiveLightListIndex) {
                PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[ActiveLightListIndex]);
                float Weight = EstimateLightContribution(L, WorldPosition, WorldNormal);
                if (Weight > 0.f) {
                    // Add the light to the sampler
                    AddLightToSampler(LS, Weight, ActiveLightListIndex);
                    NumNonZeroGridLights ++;
                }
                LightListIndex++;
            }
            Iteration++;
        }
    }
    float SumResampleWeights = 0.f, SumTargetWeigts = 0.f;
    float U = R.rand();
    uint NumValidSamples = 0;
    float3 SumResampleWeights3 = 0.f;
    LightSample ReservedSample = (LightSample)0;
    // Spawn 1 sample for each light, and resample from the samples
    for (int SamplerLightListIndex = 0; SamplerLightListIndex < NUM_LIGHT_SAMPELR_SAMPLES; SamplerLightListIndex++) {
        uint ActiveLightListIndex = LS.ActiveLightListIndex[SamplerLightListIndex];
		if(IsValid(ActiveLightListIndex)) {
            uint LightIndex = ActiveLightListBuffer[ActiveLightListIndex];
            EvaluatedLight Evaluated = EvaluateLight(LightBuffer[LightIndex]);
            float2 u2 = R.rand2();
            LightSample Sample = SampleLightDiffuseWithPreMultipliedCosine(WorldPosition, WorldNormal, Evaluated, u2);
            // Clip samples with low pdf (potential fireflies)
            if (Sample.IsValid() && Sample.Pdf > 0.001f) {
                NumValidSamples ++;
                float LightCdf = LS.Weights[SamplerLightListIndex] / LS.SumWeight;
                // Pdf of the proposal distribution (hemisphere)
                float ProposedPdf = LightCdf * Sample.Pdf;
                // Target pdf (light contribution)
                float3 TargetPdf3Unnormalized = Sample.Radiance / Sample.Pdf;
                float TargetPdfUnnormalized = dot(TargetPdf3Unnormalized, 1.f.xxx);
                // RIS
                float3 ResampleWeight3 = TargetPdf3Unnormalized / ProposedPdf;
                float ResampleWeight = TargetPdfUnnormalized / max(ProposedPdf, 1e-7f);
                if (ResampleWeight > 1e-4f) {
                    float CurrentLightU = ResampleWeight / (SumResampleWeights + ResampleWeight);
                    SumResampleWeights3 += ResampleWeight3;
                    SumResampleWeights += ResampleWeight;
                    if (CurrentLightU > U) {
                        U /= CurrentLightU;
                        ReservedSample = Sample;
                    } else {
                        U = (U - CurrentLightU) / max(1 - CurrentLightU, 1e-7f);
                    }
                }
            }
		}
    }
    if (ReservedSample.IsValid() && dot(ReservedSample.Radiance, 1.f.xxx) > 0) {
        // Final sample acquired, prepare visibility trace
        // Estimate the radiance from a single light.
        float3 RadianceEstimation = SumResampleWeights3 / NumValidSamples;
        // Account for overflowing lights that have not been injected into the grid.
        RadianceEstimation /= ListCdf;
        float3 TraceDirection = ReservedSample.Position - WorldPosition;
        float TraceDistance = length(TraceDirection);
        TraceDirection /= TraceDistance;
        // Write to the direct lighting sample buffer
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = float4(RadianceEstimation, 1.f);
        bool bPrimaryThread = WaveIsFirstLane();
        uint WaveRayCount = WaveActiveCountBits(true);
        uint WaveRayOffset = 0;
        if (bPrimaryThread) {
            InterlockedAdd(RWRayToTraceCount[0], WaveRayCount, WaveRayOffset);
        }
        WaveRayOffset = WaveReadLaneFirst(WaveRayOffset);
        uint WaveLocalRayOffset = WavePrefixCountBits(true);
        uint RayIndex = WaveRayOffset + WaveLocalRayOffset;
        // Write ray trace data
        RWRayToTraceDirectionBuffer[RayIndex] = PackNormal(TraceDirection);
        RWRayToTraceOriginScreenCoordBuffer[RayIndex] = PackUint2x16(PixelIndex);
        RWRayToTraceStateBuffer[RayIndex] = PackRayToTraceState(0.f, false);
        RWShadowRayToTraceTMaxBuffer[RayIndex] = TraceDistance * DirectLighting_UB.ShadowRayLengthMultiplier;
        
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
    if(RayIndex >= RayToTraceCount[0]) {
        return; // No rays to trace
    }

    RayToTrace RayToTrace = FetchRayToTraceWithScreenOrigin(
        RayIndex,
        min(DirectLighting_UB.ShadowRayTMax, ShadowRayToTraceTMaxBuffer[RayIndex])
    );

    CameraParameters C = GetActiveCamera();
    uint2 PixelIndex = RayToTrace.OriginScreenCoord;
    float2 PixelUV = ScreenCoordsToUV(C, PixelIndex);
    float ReversedZDepth = G_DepthTexture.SampleLevel(PointClampSampler, PixelUV, 0);
    // Screen space ray trace
    float3 Estimate = RWDirectLightingRadianceEstimateTexture[PixelIndex].rgb;
    // Shadow ray trace
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
    {
        // Offset the origin a bit, but at most 1 pixel (100%)
        float3 Normal = normalize(G_NormalTexture.SampleLevel(PointClampSampler, PixelUV, 0).xyz - 0.5f.xxx);
        float MaxOffsetLength = LinearDepth * 1e-3f;
        float2 PixelSize = GetPixelWorldSize(C, LinearDepth);
        float ProjectionX = abs(dot(C.NormalizedRight, Normal));
        float ProjectionY = abs(dot(C.NormalizedUp, Normal));
        float Fraction = 1.f;
        float MaxX = Fraction * PixelSize.x / max(ProjectionX, 1e-4f);
        float MaxY = Fraction * PixelSize.y / max(ProjectionY, 1e-4f);
        float OffsetLength = min(MaxOffsetLength, min(MaxX, MaxY));
        WorldPosition += OffsetLength * Normal;
    }

    float3 TraceDirection = RayToTrace.Direction;
    float TraceTMax = RayToTrace.TMax;
    bool bHit = false;
    float3 HitUVZ = 0, LastVisibleUVZ = 0;
    float HitTileZ = 0;
    ScreenSpaceRayTrace(
        C, G_DepthTexture, G_HiZBuffer,
        WorldPosition, TraceDirection, TraceTMax,
        40, HybridTracing_UB.SSRT_RelativeTexelThickness, 0,
        bHit, HitUVZ, LastVisibleUVZ, HitTileZ
    );

    float3 HitWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(HitUVZ.xy), ZDepthToLinearDepth(C, HitUVZ.z));
    float HitDistance = min(length(HitWorldPosition - WorldPosition), TraceTMax);

    // FIXME
    if (false && bHit) {
        // Double checking using history buffer
        float3 PreviousUVZ = ReprojectToPreviousUVZFromUVZ(C, HitUVZ);
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float Noise = InterleavedGradientNoise(UV, DirectLighting_UB.FrameIndex);
        if (all(PreviousUVZ.xy >= 0) && all(PreviousUVZ.xy < 1)) {
            // Calculate the expected depth of the pixel last frame
            float PrevZDepth = PreviousUVZ.z;

            // Lookup the actual depth at the same screen position last frame
            float ReversedHistoryZDepth = G_HistoryDepth.SampleLevel(PointClampSampler, PreviousUVZ.xy, 0).x;
            float HistoryZDepth = 1.f - ReversedHistoryZDepth;

            bHit = abs(HistoryZDepth - PrevZDepth) < HybridTracing_UB.SSRT_RelativeTexelThickness * 0.5f * lerp(.5f, 2.0f, Noise);
        }
    }

    if (!bHit) {
        // Not occluded, prepare for world trace.
        // Backward the ray a little from the last visible position for ray continuation
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

Texture2D<float>  VolumeDensityTexture;
Texture2D<float2> VolumeMinMaxTexture;
Texture2D<float4> VolumeColorTexture;
Texture2D<float2> VolumeCdfAttenuationTexture;


struct PixelVolume {
    float Min, Max;
    float Density;
    float3 Color;
    float Cdf;
    float Attenuation;
};

PixelVolume FetchVolume(float2 UV) {
    float2 MinMax = VolumeMinMaxTexture.SampleLevel(PointClampSampler, UV, 0).xy;
    PixelVolume Volume;
    Volume.Min = MinMax.x;
    Volume.Max = MinMax.y;
    Volume.Density = VolumeDensityTexture.SampleLevel(PointClampSampler, UV, 0).x;
    Volume.Color = VolumeColorTexture.SampleLevel(PointClampSampler, UV, 0).xyz;
    float2 CdfAndAttenuation = VolumeCdfAttenuationTexture.SampleLevel(PointClampSampler, UV, 0).xy;
    Volume.Cdf = CdfAndAttenuation.x;
    Volume.Attenuation = CdfAndAttenuation.y;
    return Volume;
}

// Volume samples
Texture2D<float4> VolumeSampleColorAndLinearDepth;
Texture2D<float2> VolumeSampleTransmittanceAndPdf;

// DI textures
RWTexture2D<float4> RWVolumeDirectLightingRadianceEstimateTexture;
Texture2D<float4> VolumeDirectLightingRadianceEstimateTexture;

// Volume rays
RWStructuredBuffer<uint> RWVolumeRayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWVolumeRayToTraceStateBuffer;
RWStructuredBuffer<float3> RWVolumeRayToTraceOriginBuffer;
RWStructuredBuffer<float> RWVolumeRayToTraceTMaxBuffer;

RWStructuredBuffer<uint> RWVolumeRayToTracePixelIndexBuffer;

// Trace results
StructuredBuffer<float> VolumeRayToTraceTransmittanceBuffer;

// Output lighting
RWTexture2D<float4> RWVolumeDirectLightingTexture;


// Dispatch a thread for each tile
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void VolumePrimitivesSpawnLightSamples(uint2 GroupID: SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelIndex = GroupID * TILE_SIZE + LocalID;
    if (any(PixelIndex >= View.Camera.FilmDimensions)) return;

    CameraParameters C = GetActiveCamera();
    float2 PixelUV = ScreenCoordsToUV(C, PixelIndex);
    float ReversedZDepth = G_DepthTexture.SampleLevel(PointClampSampler, PixelUV, 0);
    if (ReversedZDepth == 0) {
        RWVolumeDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return; // Skip empty pixels
    }

    float4 ColorAndLinearDepth = VolumeSampleColorAndLinearDepth.SampleLevel(PointClampSampler, PixelUV, 0);
    float2 TransmittanceAndPdf = VolumeSampleTransmittanceAndPdf.SampleLevel(PointClampSampler, PixelUV, 0);
    if(TransmittanceAndPdf.y == 0) {
        // No valid volume sample found. No need to spawn light samples for it.
        RWVolumeDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return ;
    }
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, ColorAndLinearDepth.w);
    float3 ViewDirection = WorldPosition - C.Position;
    uint4 GridIndex = LightGrid_GetGridIndex(WorldPosition);
    if (!IsValid(GridIndex.x)) {
        RWVolumeDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return; // Out of light grid
    }
    uint GridIndex1 = LightGrid_GetGridIndex1(GridIndex);

    Random R = MakeRandom(46315198u + PixelIndex.x + PixelIndex.y * 5839, LightStructure_UB.FrameIndex);
    LightSampler LS = InitLightSampler(R);

    bool bUniformGrid = WaveActiveAllEqual(GridIndex1);
    float GridSize = 0;
    float3 GridMin = LightGrid_GetGridBounds(GridIndex, GridSize);
    uint NumGridLights = LightGrid_GridLightListLengthBuffer[GridIndex1];
    uint GridLightListOffset = LightGrid_GridLightListOffsetBuffer[GridIndex1];
    float ListCdf = LightGrid_GridLightListCdfBuffer[GridIndex1];
    
    uint NumNonZeroGridLights = 0;
    if (bUniformGrid) {
        for (uint LightListIndex = 0; LightListIndex < NumGridLights; LightListIndex++) {
            uint ActiveLightListIndex = LightGrid_ListLightIndexBuffer[GridLightListOffset + LightListIndex];
            PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[ActiveLightListIndex]);
            float Weight = EstimateLightContribution(L, WorldPosition, ViewDirection, true);
            if(Weight > 0.f) {
                AddLightToSampler(LS, Weight, ActiveLightListIndex);
                NumNonZeroGridLights ++;
            }
        }
    } else {
        // Process the light with the minimum index in the grid
        uint LightListIndex = 0, Iteration = 0;
        // TODO remove Iteration (used to prevent driver timeouts)
        // TODO add a noisy occlusion modifier based on history cache to the target distribution
        while(LightListIndex < NumGridLights && Iteration < 256) {
            uint ActiveLightListIndex = INVALID_UINT;
            ActiveLightListIndex = LightGrid_ListLightIndexBuffer[GridLightListOffset + LightListIndex];
            uint WaveMinLightIndex = WaveActiveMin(ActiveLightListIndex);
            if (WaveMinLightIndex == ActiveLightListIndex) {
                PrecomputedLight L = UnpackPrecomputedLight(PrecomputedActiveLightBuffer[ActiveLightListIndex]);
                float Weight = EstimateLightContribution(L, WorldPosition, ViewDirection, true);
                if (Weight > 0.f) {
                    // Add the light to the sampler
                    AddLightToSampler(LS, Weight, ActiveLightListIndex);
                    NumNonZeroGridLights ++;
                }
                LightListIndex++;
            }
            Iteration++;
        }
    }
    float SumResampleWeights = 0.f, SumTargetWeigts = 0.f;
    float U = R.rand();
    uint NumValidSamples = 0;
    float3 SumResampleWeights3 = 0.f;
    LightSample ReservedSample = (LightSample)0;
    // Simply assume all volumes have the same isotropic parameter g
    float g = 0.5f;
    // Spawn 1 sample for each light, and resample from the samples
    for (int SamplerLightListIndex = 0; SamplerLightListIndex < NUM_LIGHT_SAMPELR_SAMPLES; SamplerLightListIndex++) {
        uint ActiveLightListIndex = LS.ActiveLightListIndex[SamplerLightListIndex];
		if(IsValid(ActiveLightListIndex)) {
            uint LightIndex = ActiveLightListBuffer[ActiveLightListIndex];
            EvaluatedLight Evaluated = EvaluateLight(LightBuffer[LightIndex]);
            float2 u2 = R.rand2();
            LightSample Sample = SampleLightWithPreMultipliedPhaseFunction(
                WorldPosition, ViewDirection, Evaluated, g, u2
            );
            // Clip samples with low pdf (potential fireflies)
            if (Sample.IsValid() && Sample.Pdf > 0.001f) {
                NumValidSamples ++;
                float LightCdf = LS.Weights[SamplerLightListIndex] / LS.SumWeight;
                // Pdf of the proposal distribution (hemisphere)
                float ProposedPdf = LightCdf * Sample.Pdf;
                // Target pdf (light contribution)
                float3 TargetPdf3Unnormalized = Sample.Radiance / Sample.Pdf;
                float TargetPdfUnnormalized = dot(TargetPdf3Unnormalized, 1.f.xxx);
                // RIS
                float3 ResampleWeight3 = TargetPdf3Unnormalized / ProposedPdf;
                float ResampleWeight = TargetPdfUnnormalized / max(ProposedPdf, 1e-7f);
                if (ResampleWeight > 1e-4f) {
                    float CurrentLightU = ResampleWeight / (SumResampleWeights + ResampleWeight);
                    SumResampleWeights3 += ResampleWeight3;
                    SumResampleWeights += ResampleWeight;
                    if (CurrentLightU > U) {
                        U /= CurrentLightU;
                        ReservedSample = Sample;
                    } else {
                        U = (U - CurrentLightU) / max(1 - CurrentLightU, 1e-7f);
                    }
                }
            }
		}
    }
    if (ReservedSample.IsValid() && dot(ReservedSample.Radiance, 1.f.xxx) > 0) {
        // Final sample acquired, prepare visibility trace
        // Estimate the radiance from a single light.
        float3 RadianceEstimation = SumResampleWeights3 / NumValidSamples;
        // Account for overflowing lights that have not been injected into the grid.
        RadianceEstimation /= ListCdf;
        float3 TraceDirection = ReservedSample.Position - WorldPosition;
        float TraceDistance = length(TraceDirection);
        TraceDirection /= TraceDistance;
        // Write to the direct lighting sample buffer
        RWVolumeDirectLightingRadianceEstimateTexture[PixelIndex] = float4(RadianceEstimation, 1.f);
        bool bPrimaryThread = WaveIsFirstLane();
        uint WaveRayCount = WaveActiveCountBits(true);
        uint WaveRayOffset = 0;
        if (bPrimaryThread) {
            InterlockedAdd(RWVolumeRayToTraceCount[0], WaveRayCount, WaveRayOffset);
        }
        WaveRayOffset = WaveReadLaneFirst(WaveRayOffset);
        uint WaveLocalRayOffset = WavePrefixCountBits(true);
        uint RayIndex = WaveRayOffset + WaveLocalRayOffset;
        // Write ray trace data
        RWVolumeRayToTraceDirectionBuffer[RayIndex] = PackNormal(TraceDirection);
        RWVolumeRayToTraceStateBuffer[RayIndex] = PackRayToTraceState(0.f, false);
        RWVolumeRayToTraceTMaxBuffer[RayIndex] = TraceDistance * DirectLighting_UB.ShadowRayLengthMultiplier;
        
        // Specify the pixel index for each transmittance ray
        RWVolumeRayToTracePixelIndexBuffer[RayIndex] = PackUint2x16(PixelIndex);
    }
    else {
        RWVolumeDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
    }
}

// HWRT transmittance ray tracing...


RayToTrace FetchVolumeRayToTraceWithWorldOrigin(uint RayIndex, float TMax) {
    RayToTrace Ray = (RayToTrace)0;
    Ray.Origin = RWVolumeRayToTraceOriginBuffer[RayIndex];
    Ray.Direction = UnpackNormal(RWVolumeRayToTraceDirectionBuffer[RayIndex]);
    uint RayToTraceState = RWVolumeRayToTraceStateBuffer[RayIndex];
    Ray.TMax = TMax;
    Ray.TCurrent = UnpackRayToTraceState(RayToTraceState, Ray.bHit);
    return Ray;
}

// Render direct lighting for volume primitives using trace results
[numthreads(WAVE_SIZE, 1, 1)]
void RenderVolumeDirectLighting(uint DispatchThreadID : SV_DispatchThreadID)
{
    uint RayIndex = DispatchThreadID;
    if(RayIndex >= RWVolumeRayToTraceCount[0]) return;
    RayToTrace RayToTrace = FetchVolumeRayToTraceWithWorldOrigin(RayIndex, 0); 
    float RayTransmittance = VolumeRayToTraceTransmittanceBuffer[RayIndex];
    if (!RayToTrace.bHit) {
        CameraParameters C = GetActiveCamera();
        uint2 PixelIndex = UnpackUint2x16(RWVolumeRayToTracePixelIndexBuffer[RayIndex]);
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float3 Estimate = VolumeDirectLightingRadianceEstimateTexture.SampleLevel(PointClampSampler, UV, 0).rgb;
        float3 Radiance = RayTransmittance * Estimate;
        // Resemble volume sampling
        float3 VolumeSampleColor = VolumeSampleColorAndLinearDepth.SampleLevel(PointClampSampler, UV, 0).rgb;
        float2 VolumeSampleTransmittancePdf = VolumeSampleTransmittanceAndPdf.SampleLevel(PointClampSampler, UV, 0);
        float  VolumeSampleTransmittance = VolumeSampleTransmittancePdf.x;
        float  VolumeSamplePdf = VolumeSampleTransmittancePdf.y;
        Radiance = Radiance * VolumeSampleColor * VolumeSampleTransmittance / VolumeSamplePdf;
        RWVolumeDirectLightingTexture[PixelIndex] = float4(Radiance, 1.f);
    }
}