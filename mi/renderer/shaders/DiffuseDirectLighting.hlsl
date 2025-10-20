#include "shared/SharedView.hlsl"
#include "shared/SharedDebug.hlsl"
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
#include "resources/LightGrid.hlsl"
#include "resources/LightEvaluation.hlsl"
#include "resources/LightGridSampling.hlsl"

RWStructuredBuffer<uint> RWRayToTraceCount;
RWStructuredBuffer<uint> RWVolumeRayToTraceCount;

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

// Sometimes when involving ray compaction / continuation, allocate new rays on this buffer
RWStructuredBuffer<uint> RWRayToTraceListAllocator;
RWStructuredBuffer<uint> RWRayToTraceListBuffer;

RWStructuredBuffer<uint> RWRayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWRayToTraceStateBuffer;

// Optional (when the starting point is exactly on a pixel center)
RWStructuredBuffer<uint> RWRayToTraceOriginScreenCoordBuffer;

// Optional (when the ray origin is in world space)
RWStructuredBuffer<float3> RWRayToTraceOriginBuffer;

// Special sampler used for SSRT
SamplerState PointBorder1Sampler;

ConstantBuffer<DirectLightingUB> DirectLighting_UB;

struct HybridTracingUB {
    uint  SSRT_Disabled;
    float SSRT_RelativeTexelThickness;
    float RayContinuationBackwardBiasFactor;
    float DefaultTMax;
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
        LightGrid_ListAllocator[0] = 0;
        LightGrid_ActiveLightListCount[0] = 0;
        RWRayToTraceCount[0] = 0;
        RWRayToTraceListAllocator[0] = 0;
        RWVolumeRayToTraceCount[0] = 0;
    }
    uint Index = DispatchID;
    if (Index >= LightStructure_UB.LighGridNumCascadesUsed * LightStructure_UB.LightGridNumGrids) {
        return;
    }
     LightGrid_GridLightListLengthBuffer[Index] = 0;
}

// Precompute lights, filter active lights and gather light data for later injection
[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void PrecomputeLights(uint DispatchID: SV_DispatchThreadID) {
    uint LightIndex = DispatchID;
    if (LightIndex >= LightStructure_UB.MaxNumLights) return;
    AreaLight LightData = LightBuffer[LightIndex];
    if (LightData.Flags == 0) return; // Invalid light, skip
    // Extract light data
    EvaluatedAreaLight Evaluated = EvaluateLight(LightData);
    // Evaluate other features
    float3 N = normalize(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0));
    // Precompute
    {
        PrecomputedLight L = (PrecomputedLight)0;
        L.V0 = Evaluated.V0;
        L.V1 = Evaluated.V1;
        L.V2 = Evaluated.V2;
        L.Normal = N;
        L.Hash = GetExpandedLightHash64(LightIndex, GetLightHash32(LightData));
        float Area = length(cross(Evaluated.V1 - Evaluated.V0, Evaluated.V2 - Evaluated.V0)) * 0.5f;
        L.Intensity = RadianceToLuminance(Evaluated.EstimatedAverageEmission) * Area;
        if (L.Intensity > 1e-5f) {
            // Allocate active light list
            uint WaveNumActiveLights = WaveActiveCountBits(true);
            uint WaveLightListOffset = 0;
            if (WaveIsFirstLane()) {
                InterlockedAdd(LightGrid_ActiveLightListCount[0], WaveNumActiveLights, WaveLightListOffset);
            }
            WaveLightListOffset = WaveReadLaneFirst(WaveLightListOffset);
            uint WaveLightListIndex = WavePrefixCountBits(true);
            uint LightListIndex = WaveLightListOffset + WaveLightListIndex;
            // Precompute and store active lights
            LightGrid_ActiveLightListBuffer[LightListIndex] = LightIndex;
            LightGrid_PrecomputedActiveLightBuffer[LightListIndex] = PackPrecomputedLight(L);
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
    uint NumActiveLights = LightGrid_ActiveLightListCount[0];
    uint NumGridLights = 0, WriteLocation = 0;
    uint SampledOffset = 0, CandidateOffset = MAX_NUM_GRID_LIGHTS;
    Random R = MakeRandom(17419142u + DispatchID, LightStructure_UB.FrameIndex);
    float U = R.rand();
    float SumWeights = 0.0f, SumSampledWeights = 0.f, SumCandidateWeights = 0.f;
    for (uint LightListIndex = 0; LightListIndex < NumActiveLights; LightListIndex++) {
        PrecomputedLight L = UnpackPrecomputedLight(LightGrid_PrecomputedActiveLightBuffer[LightListIndex]);
        float Weight = LightGrid_EstimateLightGridContribution(L, GridMin, GridSize);
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
    LightGrid_GridLightListLengthBuffer[GridIndex1] = NumSampledLights;
    LightGrid_GridLightListCdfBuffer[GridIndex1] = SumSampledWeights / max(SumWeights, 1e-6f);
    if (LocalID == 0) SharedListElementsRequired = 0;
    GroupMemoryBarrierWithGroupSync();
    uint LocalOffset = 0;
    InterlockedAdd(SharedListElementsRequired, NumSampledLights, LocalOffset);
    GroupMemoryBarrierWithGroupSync();
    if (LocalID == 0) {
        InterlockedAdd(LightGrid_ListAllocator[0], SharedListElementsRequired, SharedListOffsetBase);
    }
    GroupMemoryBarrierWithGroupSync();
    uint GlobalOffset = SharedListOffsetBase + LocalOffset;
    LightGrid_GridLightListOffsetBuffer[GridIndex1] = GlobalOffset;
    for (uint i = 0; i < NumSampledLights; i++) {
        uint LightListIndex = SharedGridLightListIndices[(SampledOffset + i) * WAVE_SIZE + LocalID];
        // This time we store active light list index in the grid buffer 
        LightGrid_ListActiveLightListIndexBuffer[GlobalOffset + i] = LightListIndex;
    }
}

Texture2D<float> G_DepthTexture;
Texture2D<float4> G_NormalTexture;

RWStructuredBuffer<float> RWShadowRayToTraceTMaxBuffer;
RWStructuredBuffer<uint>  RWShadowRayToTraceSampledLightIndexBuffer;
StructuredBuffer<float> ShadowRayToTraceTMaxBuffer;
StructuredBuffer<float> ShadowRayToTraceTransmittanceBuffer;


// Direction (Normal uint packed), Length (float)
RWTexture2D<uint> RWDirectLightingRayIndexTexture; // Specify the shadow ray index in the following hybrid tracing process
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDirectLightingRadianceEstimateTexture; // Output buffer for direct lighting radiance estimates

Texture2D<float> G_HiZBuffer;
Texture2D<float> G_HistoryDepthTexture;

Texture2D<float4> DirectLightingRadianceEstimateTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDiffuseDirectLightingTexture;

// Dispatch a thread for each tile
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void SpawnLightSamples(uint2 GroupID: SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelIndex = GroupID * TILE_SIZE + LocalID;
    if (any(PixelIndex >= View.Camera.FilmDimensions)) return;

    CameraParameters C = GetActiveCamera();
    float2 PixelUV = ScreenCoordsToUV(C, PixelIndex);
    float ReversedZDepth = G_DepthTexture.SampleLevel(PointEdgeSampler, PixelUV, 0);
    if (ReversedZDepth == 0) {
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return; // Skip empty pixels
    }

    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
    float3 WorldNormal = normalize(G_NormalTexture.SampleLevel(PointEdgeSampler, PixelUV, 0).xyz - 0.5f.xxx);
    Random R = MakeRandom(32618420u + PixelIndex.x + PixelIndex.y * 5839, LightStructure_UB.FrameIndex);
    float3 SumResampleWeights3 = 0.f;
    uint NumValidSamples = 0;
    float LightGridLightListCdf = 1.f;
    float3 ViewDirection = normalize(C.Position - WorldPosition);
    LightSample ReservedSample = SampleOneLightSample_RIS(
        WorldPosition, WorldNormal, ViewDirection,
        true, true, false, 
        R, SumResampleWeights3, NumValidSamples,
        LightGridLightListCdf
    );
    if (ReservedSample.IsValid() && dot(ReservedSample.Radiance, 1.f.xxx) > 0) {
        // Final sample acquired, prepare visibility trace
        // Estimate the radiance from a single light.
        float3 RadianceEstimation = SumResampleWeights3 / NumValidSamples;
        // Account for overflowing lights that have not been injected into the grid.
        RadianceEstimation /= LightGridLightListCdf;
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
        // Keep sampled light index for visibility update
        RWShadowRayToTraceSampledLightIndexBuffer[RayIndex] = ReservedSample.LightIndex;
        
        RWDirectLightingRayIndexTexture[PixelIndex] = RayIndex;
    }
    else {
        RWDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
    }
}

StructuredBuffer<uint> RayToTraceCount;

Texture2D<uint> G_FlagsTexture;
Texture2D<uint> OrFlagsTexture;

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
    float ReversedZDepth = G_DepthTexture.SampleLevel(PointEdgeSampler, PixelUV, 0);
    // Screen space ray trace
    float3 Estimate = RWDirectLightingRadianceEstimateTexture[PixelIndex].rgb;
    // Shadow ray trace
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
    
    {
        // Offset the origin a bit, but at most 0.45 pixel (45%)
        float3 Normal = normalize(G_NormalTexture.SampleLevel(PointEdgeSampler, PixelUV, 0).xyz - 0.5f.xxx);
        float MaxOffsetLength = LinearDepth * 1e-3f;
        float2 PixelSize = GetPixelWorldSize(C, LinearDepth);
        float ProjectionX = abs(dot(C.NormalizedRight, Normal));
        float ProjectionY = abs(dot(C.NormalizedUp, Normal));
        float Fraction = 0.45f;
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
    float3 LastValidUVZ = 0;
    bool DebugFlag = false; //all(PixelIndex == Debug.CursorScreenCoords);
    ScreenSpaceRayTrace(
        C, G_DepthTexture, G_HiZBuffer, G_FlagsTexture, OrFlagsTexture, PointBorder1Sampler,
        WorldPosition, TraceDirection, TraceTMax,
        40, HybridTracing_UB.SSRT_RelativeTexelThickness, 0,
        DebugFlag,
        bHit, HitUVZ, LastVisibleUVZ, HitTileZ, LastValidUVZ
    );

    float3 HitWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(HitUVZ.xy), ReversedZDepthToLinearDepth(C, HitUVZ.z));
    float HitDistance = min(length(HitWorldPosition - WorldPosition), TraceTMax);

    if(HybridTracing_UB.SSRT_Disabled) {
        bHit = false;
    }

    if (!HybridTracing_UB.SSRT_Disabled && bHit) {
        // Double checking using history buffer
        float3 PreviousUVZ = ReprojectToPreviousUVZFromUVZ(C, float3(HitUVZ.xy, 1 - HitUVZ.z));
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float Noise = InterleavedGradientNoise(UV, DirectLighting_UB.FrameIndex);
        if (all(PreviousUVZ.xy >= 0) && all(PreviousUVZ.xy < 1)) {
            // Calculate the expected depth of the pixel last frame
            float PrevZDepth = PreviousUVZ.z;

            // Lookup the actual depth at the same screen position last frame
            float ReversedHistoryZDepth = G_HistoryDepthTexture.SampleLevel(PointEdgeSampler, PreviousUVZ.xy, 0).x;
            float HistoryZDepth = 1.f - ReversedHistoryZDepth;

            bHit = abs(HistoryZDepth - PrevZDepth) < HybridTracing_UB.SSRT_RelativeTexelThickness * 0.5f * lerp(.5f, 2.0f, Noise);
        }
    }

    if (!bHit) {
        // Not occluded, prepare for world trace and transmittance calculation.
        // Backward the ray a little from the last valid (and visible) position for ray continuation
        float LinearDepth = ReversedZDepthToLinearDepth(C, LastValidUVZ.z);
        float3 LastValidWorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(LastValidUVZ.xy), LinearDepth);
        HitDistance = min(length(LastValidWorldPosition - WorldPosition), TraceTMax);

        float Bias = min(LinearDepth * HybridTracing_UB.RayContinuationBackwardBiasFactor, HitDistance * 0.5f);
        HitDistance = max(HitDistance - Bias, 0);
        
        if(HybridTracing_UB.SSRT_Disabled) {
            HitDistance = 1e-4f;
        }

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

RWStructuredBuffer<uint> RWDebugTracedRaysCount;
RWStructuredBuffer<float3> RWDebugTracedRayOrigins;
RWStructuredBuffer<uint> RWDebugTracedRayDirections;
RWStructuredBuffer<uint> RWDebugTracedRayStates;

// Render diffuse direct lighting using trace results
// 1 thread per ray
[numthreads(WAVE_SIZE, 1, 1)]
void RenderDiffuseDirectLighting(uint DispatchThreadID : SV_DispatchThreadID)
{
    uint RayIndex = DispatchThreadID;
    if(RayIndex >= RWRayToTraceCount[0]) return;
    RayToTrace RayToTrace = FetchRayToTraceWithScreenOrigin(RayIndex, 0);
    CameraParameters C = GetActiveCamera();
    uint2 PixelIndex = RayToTrace.OriginScreenCoord;
    if (!RayToTrace.bHit) {
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float3 Estimate = DirectLightingRadianceEstimateTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;
        float Transmittance = ShadowRayToTraceTransmittanceBuffer[RayIndex];
        // Not multiplied by BSDF (multiplied later in the final composition pass)
        RWDiffuseDirectLightingTexture[PixelIndex] = float4(Estimate * Transmittance, 1.f);
        // Update grid visibility bloom filter
        {
            float ReversedZDepth = G_DepthTexture.SampleLevel(PointEdgeSampler, UV, 0).x;
            float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
            float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
            uint LightIndex = RWShadowRayToTraceSampledLightIndexBuffer[RayIndex];
            LightGrid_UpdateVisibilityForAreaLight(WorldPosition, LightIndex);
        }
    }
#ifdef DEBUG_OUTPUT_TRACED_RAY
    if (all(PixelIndex == Debug.CursorScreenCoords)) {
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float ReversedZDepth = G_DepthTexture.SampleLevel(PointEdgeSampler, UV, 0).x;
        float3 WorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(UV), ReversedZDepthToLinearDepth(C, ReversedZDepth));
        RWDebugTracedRaysCount[0] = 1;
        RWDebugTracedRayDirections[0] = PackNormal(RayToTrace.Direction);
        RWDebugTracedRayOrigins[0] = WorldPosition;
        RWDebugTracedRayStates[0] = PackRayToTraceState(RayToTrace.TCurrent, true);
    }
#endif
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
    float2 MinMax = VolumeMinMaxTexture.SampleLevel(PointEdgeSampler, UV, 0).xy;
    PixelVolume Volume;
    Volume.Min = MinMax.x;
    Volume.Max = MinMax.y;
    Volume.Density = VolumeDensityTexture.SampleLevel(PointEdgeSampler, UV, 0).x;
    Volume.Color = VolumeColorTexture.SampleLevel(PointEdgeSampler, UV, 0).xyz;
    float2 CdfAndAttenuation = VolumeCdfAttenuationTexture.SampleLevel(PointEdgeSampler, UV, 0).xy;
    Volume.Cdf = CdfAndAttenuation.x;
    Volume.Attenuation = CdfAndAttenuation.y;
    return Volume;
}

// Volume samples
Texture2D<float4> VolumeSampleColorAndLinearDepth;
Texture2D<float2> VolumeSampleTransmittanceAndPdf;

// DI textures
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeDirectLightingRadianceEstimateTexture;
Texture2D<float4> VolumeDirectLightingRadianceEstimateTexture;

// Volume rays
RWStructuredBuffer<uint> RWVolumeRayToTraceDirectionBuffer;
RWStructuredBuffer<uint> RWVolumeRayToTraceStateBuffer;
RWStructuredBuffer<float3> RWVolumeRayToTraceOriginBuffer;
RWStructuredBuffer<float> RWVolumeRayToTraceTMaxBuffer;

RWStructuredBuffer<uint> RWVolumeRayToTracePixelIndexBuffer;

RWStructuredBuffer<uint> RWVolumeRayToTraceSampledLightIndexBuffer;

// Trace results
StructuredBuffer<float> VolumeRayToTraceTransmittanceBuffer;

// Output lighting
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeDirectLightingTexture;


// Dispatch a thread for each tile
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void VolumePrimitivesSpawnLightSamples(uint2 GroupID: SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelIndex = GroupID * TILE_SIZE + LocalID;
    if (any(PixelIndex >= View.Camera.FilmDimensions)) return;
    
    RWVolumeDirectLightingTexture[PixelIndex] = 0.f.xxxx; // Initialize the output texture

    CameraParameters C = GetActiveCamera();
    float2 PixelUV = ScreenCoordsToUV(C, PixelIndex);

    float4 ColorAndLinearDepth = VolumeSampleColorAndLinearDepth.SampleLevel(PointEdgeSampler, PixelUV, 0);
    float2 TransmittanceAndPdf = VolumeSampleTransmittanceAndPdf.SampleLevel(PointEdgeSampler, PixelUV, 0);
    if(TransmittanceAndPdf.y == 0) {
        // No valid volume sample found. No need to spawn light samples for it.
        RWVolumeDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return ;
    }
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, ColorAndLinearDepth.w);
    float3 ViewDirection = normalize(C.Position - WorldPosition);
    Random R = MakeRandom(46315198u + PixelIndex.x + PixelIndex.y * 5839, LightStructure_UB.FrameIndex);
    float3 SumResampleWeights3 = 0.f;
    uint   NumValidSamples = 0;
    float  LightGridLightListCdf = 0;
    LightSample ReservedSample = SampleOneLightSample_RIS(
        WorldPosition, 0.xxx, ViewDirection,
        false, true, false,
        R, SumResampleWeights3, NumValidSamples, LightGridLightListCdf
    );
    if (ReservedSample.IsValid() && dot(ReservedSample.Radiance, 1.f.xxx) > 0) {
        // Final sample acquired, prepare visibility trace
        // Estimate the radiance from a single light.
        float3 RadianceEstimation = SumResampleWeights3 / NumValidSamples;
        // Account for overflowing lights that have not been injected into the grid.
        RadianceEstimation /= LightGridLightListCdf;
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
        RWVolumeRayToTraceOriginBuffer[RayIndex] = WorldPosition;
        RWVolumeRayToTraceDirectionBuffer[RayIndex] = PackNormal(TraceDirection);
        RWVolumeRayToTraceStateBuffer[RayIndex] = PackRayToTraceState(0.f, false);
        RWVolumeRayToTraceTMaxBuffer[RayIndex] = TraceDistance * DirectLighting_UB.ShadowRayLengthMultiplier;
        
        // Specify the pixel index for each transmittance ray
        RWVolumeRayToTracePixelIndexBuffer[RayIndex] = PackUint2x16(PixelIndex);
        // Keep sampled light index for visibility update
        RWVolumeRayToTraceSampledLightIndexBuffer[RayIndex] = ReservedSample.LightIndex;
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
    uint2 PixelIndex = UnpackUint2x16(RWVolumeRayToTracePixelIndexBuffer[RayIndex]);
    float RayTransmittance = VolumeRayToTraceTransmittanceBuffer[RayIndex];

    if (!RayToTrace.bHit) {
        CameraParameters C = GetActiveCamera();
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float3 Estimate = VolumeDirectLightingRadianceEstimateTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;
        float3 Radiance = RayTransmittance * Estimate;
        // Resemble volume sampling
        float3 VolumeSampleColor = VolumeSampleColorAndLinearDepth.SampleLevel(PointEdgeSampler, UV, 0).rgb;
        float2 VolumeSampleTransmittancePdf = VolumeSampleTransmittanceAndPdf.SampleLevel(PointEdgeSampler, UV, 0);
        float  VolumeSampleTransmittance = VolumeSampleTransmittancePdf.x;
        float  VolumeSamplePdf = VolumeSampleTransmittancePdf.y;
        Radiance = Radiance * VolumeSampleColor;
        // Pdf canceled out with transmittance and scattering coefficient. No need to divide it here.
        // VolumeSampleTransmittance / VolumeSamplePdf;
        RWVolumeDirectLightingTexture[PixelIndex] = float4(Radiance, 1.f);

        // Update grid visibility bloom filter
        {
            float ReversedZDepth = G_DepthTexture.SampleLevel(PointEdgeSampler, UV, 0).x;
            float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
            float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, LinearDepth);
            uint LightIndex = RWVolumeRayToTraceSampledLightIndexBuffer[RayIndex];
            LightGrid_UpdateVisibilityForAreaLight(WorldPosition, LightIndex);
        }
    }
}