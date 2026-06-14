#include "shared/SharedView.hlsl"
#include "shared/SharedDebug.hlsl"
#include "shared/SharedRenderable.hlsl"
#include "shared/SharedVertex.hlsl"
#include "shared/SharedMaterial.hlsl"
#include "shared/SharedLight.hlsl"
#include "shared/SharedVolumeGrid.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Transform.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Math.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "headers/Random.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Light.hlsl"
#include "headers/ScreenSpaceRayTracing.hlsl"
#include "headers/HybridTracing.hlsl"
#include "headers/VolumeGridLib.hlsl"

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

RWStructuredBuffer<float3> RWRayToTraceDirectionBuffer;
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
    Ray.Direction = RWRayToTraceDirectionBuffer[RayIndex];
    uint RayToTraceState = RWRayToTraceStateBuffer[RayIndex];
    Ray.TMax = TMax;
    Ray.TCurrent = UnpackRayToTraceState(RayToTraceState, Ray.bHit);
    return Ray;
}

RayToTrace FetchRayToTraceWithScreenOrigin(uint RayIndex, float TMax) {
    RayToTrace Ray = (RayToTrace)0;
    Ray.OriginScreenCoord = UnpackUint2x16(RWRayToTraceOriginScreenCoordBuffer[RayIndex]);
    Ray.Direction = RWRayToTraceDirectionBuffer[RayIndex];
    uint RayToTraceState = RWRayToTraceStateBuffer[RayIndex];
    Ray.TMax = TMax;
    Ray.TCurrent = UnpackRayToTraceState(RayToTraceState, Ray.bHit);
    return Ray;
}

[numthreads(1, 1, 1)]
void DiffuseDirectLightingClearCounters () {
    RWRayToTraceCount[0] = 0;
    RWRayToTraceListAllocator[0] = 0;
}
Texture2D<float> G_DepthTexture;
Texture2D<float4> G_NormalTexture;
Texture2D<uint> G_GeometryNormal;

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
    float3 WorldGeometryNormal = UnpackGeometryNormal(G_GeometryNormal.Load(uint3(PixelIndex, 0)).x);
    Random R = MakeRandom(32618420u + PixelIndex.x + PixelIndex.y * 5839, LightStructure_UB.FrameIndex);
    float SumResampleWeights = 0.f;
    uint NumValidSamples = 0;
    float LightGridLightListCdf = 1.f;
    float3 ViewDirection = normalize(C.Position - WorldPosition);
    float3 RadianceEstimation = 0;
    LightSample ReservedSample = SampleOneLightSample_RIS(
        WorldPosition, WorldNormal, ViewDirection,
        true, true, false, true,
        R,
        RadianceEstimation,
        SumResampleWeights, NumValidSamples,
        LightGridLightListCdf
    );
    // Reject using sample weights and geometry normal
    float3 LightSampleDirection = ReservedSample.IsInfiniteLight() ? ReservedSample.Position : normalize(ReservedSample.Position - WorldPosition);
    bool bValidSample = ReservedSample.IsValid() && dot(RadianceEstimation, 1.f.xxx) > 0 && dot(WorldGeometryNormal, LightSampleDirection) > 0;
    if (bValidSample) {
        float3 TraceDirection;
        float  TraceDistance;
        if(ReservedSample.IsInfiniteLight()) {
            TraceDirection = ReservedSample.Position;
            TraceDistance = C.FarPlane;
        } else {
            TraceDirection = normalize(ReservedSample.Position - WorldPosition);
            TraceDistance = length(ReservedSample.Position - WorldPosition);
        }
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
        RWRayToTraceDirectionBuffer[RayIndex] = TraceDirection;
        RWRayToTraceOriginScreenCoordBuffer[RayIndex] = PackUint2x16(PixelIndex);
        RWRayToTraceStateBuffer[RayIndex] = PackRayToTraceState(0.f, false);
        RWShadowRayToTraceTMaxBuffer[RayIndex] = TraceDistance * DirectLighting_UB.ShadowRayLengthMultiplier;
        // Keep sampled light record for visibility update.
        RWShadowRayToTraceSampledLightIndexBuffer[RayIndex] = ReservedSample.LightRecord.Packed;
        
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
    // Shadow ray trace
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 GeometryNormal = UnpackGeometryNormal(G_GeometryNormal.SampleLevel(PointEdgeSampler, PixelUV, 0).x);
    // Recover an offseted world position to avoid self-intersection. The offset is adequate for trimming self-intersections and moves at most .45 pixel in screen space.
    float3 WorldPosition = RecoverOffsetedWorldPositionFromScreenPixel(C, PixelIndex, LinearDepth, GeometryNormal, 0.45f);

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
        float Noise = InterleavedGradientNoise(PixelIndex + 0.5f.xx, DirectLighting_UB.FrameIndex);
        if (all(PreviousUVZ.xy >= 0) && all(PreviousUVZ.xy < 1)) {
            // Calculate the expected depth of the pixel last frame
            float PrevZDepth = PreviousUVZ.z;

            // Lookup the actual depth at the same screen position last frame
            float ReversedHistoryZDepth = G_HistoryDepthTexture.SampleLevel(PointEdgeSampler, PreviousUVZ.xy, 0).x;
            float HistoryZDepth = 1.f - ReversedHistoryZDepth;

            bHit = abs(HistoryZDepth - PrevZDepth) < HybridTracing_UB.SSRT_RelativeTexelThickness * 0.5f * lerp(.5f, 2.0f, Noise);
        }
    }

    // Compact the continuation (non-hit) rays wave-wide in UNIFORM control flow.
    // These wave ops MUST execute on every active lane, i.e. outside the divergent
    // `!bHit` branch. Inside the branch they are degenerate: `!bHit` is trivially
    // true for every lane that reaches them, so WaveActiveCountBits/WavePrefixCountBits
    // end up relying on diverged (hit) lanes being inactive -- which some drivers do
    // not honor. That scrambled the HWRT continuation list (and the transmittance
    // read-back) whenever hits and misses coexisted in a wave, producing patchy
    // light leaks only present with SSRT enabled.
    bool bNeedsContinuation = !bHit;
    uint WaveContinuationCount  = WaveActiveCountBits(bNeedsContinuation);
    uint WaveContinuationPrefix = WavePrefixCountBits(bNeedsContinuation);
    uint WaveContinuationBase   = 0;
    if (WaveIsFirstLane()) {
        InterlockedAdd(RWRayToTraceListAllocator[0], WaveContinuationCount, WaveContinuationBase);
    }
    WaveContinuationBase = WaveReadLaneFirst(WaveContinuationBase);

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

        // Claim a continuation slot using the wave-wide compaction computed above.
        uint RayListIndex = WaveContinuationBase + WaveContinuationPrefix;
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
RWStructuredBuffer<float3> RWDebugTracedRayDirections;
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
            LightSampleSrcLightRecord LightRecord = (LightSampleSrcLightRecord)0;
            LightRecord.Packed = RWShadowRayToTraceSampledLightIndexBuffer[RayIndex];
            LightGrid_UpdateVisibilityForLightRecord(WorldPosition, RayToTrace.Direction, LightRecord);
        }
    }
#ifdef DEBUG_OUTPUT_TRACED_RAY
    if (all(PixelIndex == Debug.CursorScreenCoords)) {
        float2 UV = ScreenCoordsToUV(C, PixelIndex);
        float ReversedZDepth = G_DepthTexture.SampleLevel(PointEdgeSampler, UV, 0).x;
        float3 WorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(UV), ReversedZDepthToLinearDepth(C, ReversedZDepth));
        RWDebugTracedRaysCount[0] = 1;
        RWDebugTracedRayDirections[0] = RayToTrace.Direction;
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
Texture2D<float4> VolumeSampleColorTexture;
Texture2D<float>  VolumeSampleLinearDepthTexture;
Texture2D<float2> VolumeSampleTransmittanceAndPdfTexture;

// DI textures
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeDirectLightingRadianceEstimateTexture;
Texture2D<float4> VolumeDirectLightingRadianceEstimateTexture;

// Volume rays
RWStructuredBuffer<float3> RWVolumeRayToTraceDirectionBuffer;
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

[numthreads(1, 1, 1)]
void VolumeDirectLightingClearCounters() {
    RWVolumeRayToTraceCount[0] = 0;
}

// Dispatch a thread for each tile
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void VolumeDirectLightingSpawnLightSamples(uint2 GroupID : SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelIndex = GroupID * TILE_SIZE + LocalID;
    if (any(PixelIndex >= View.Camera.FilmDimensions)) return;
    
    RWVolumeDirectLightingTexture[PixelIndex] = 0.f.xxxx; // Initialize the output texture

    CameraParameters C = GetActiveCamera();
    float2 PixelUV = ScreenCoordsToUV(C, PixelIndex);

    float  VolumeSampleLinearDepth = VolumeSampleLinearDepthTexture.SampleLevel(PointEdgeSampler, PixelUV, 0);
    float2 TransmittanceAndPdf = VolumeSampleTransmittanceAndPdfTexture.SampleLevel(PointEdgeSampler, PixelUV, 0);
    if(TransmittanceAndPdf.y == 0) {
        // No valid volume sample found. No need to spawn light samples for it.
        RWVolumeDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
        return ;
    }
    float3 WorldPosition = RecoverWorldPositionPixelCoords(C, PixelIndex, VolumeSampleLinearDepth);
    float3 ViewDirection = normalize(C.Position - WorldPosition);
    Random R = MakeRandom(46315198u + PixelIndex.x + PixelIndex.y * 5839, LightStructure_UB.FrameIndex);
    float  SumResampleWeights = 0.f;
    uint   NumValidSamples = 0;
    float  LightGridLightListCdf = 0;
    float3 RadianceEstimation = 0;
    LightSample ReservedSample = SampleOneLightSample_RIS(
        WorldPosition, 0.xxx, ViewDirection,
        false, true, false, true,
        R,
        RadianceEstimation,
        SumResampleWeights, NumValidSamples, LightGridLightListCdf
    );
    
    bool bValidSample = ReservedSample.IsValid() && dot(RadianceEstimation, 1.f.xxx) > 0;
    if (bValidSample) {
        // Final sample acquired, prepare visibility trace
        float3 TraceDirection = ReservedSample.IsInfiniteLight() ? ReservedSample.Position : (ReservedSample.Position - WorldPosition);
        float TraceDistance = ReservedSample.IsInfiniteLight() ? C.FarPlane : length(TraceDirection);
        TraceDirection /= max(TraceDistance, 1e-7f);
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
        RWVolumeRayToTraceDirectionBuffer[RayIndex] = TraceDirection;
        RWVolumeRayToTraceStateBuffer[RayIndex] = PackRayToTraceState(0.f, false);
        RWVolumeRayToTraceTMaxBuffer[RayIndex] = TraceDistance * DirectLighting_UB.ShadowRayLengthMultiplier;
        
        // Specify the pixel index for each transmittance ray
        RWVolumeRayToTracePixelIndexBuffer[RayIndex] = PackUint2x16(PixelIndex);
        // Keep sampled light record for visibility update.
        RWVolumeRayToTraceSampledLightIndexBuffer[RayIndex] = ReservedSample.LightRecord.Packed;
    }
    else {
        RWVolumeDirectLightingRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
    }
}

// HWRT transmittance ray tracing...


RayToTrace FetchVolumeRayToTraceWithWorldOrigin(uint RayIndex, float TMax) {
    RayToTrace Ray = (RayToTrace)0;
    Ray.Origin = RWVolumeRayToTraceOriginBuffer[RayIndex];
    Ray.Direction = RWVolumeRayToTraceDirectionBuffer[RayIndex];
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
        float3 VolumeSampleColor = VolumeSampleColorTexture.SampleLevel(PointEdgeSampler, UV, 0).rgb;
        float2 VolumeSampleTransmittancePdf = VolumeSampleTransmittanceAndPdfTexture.SampleLevel(PointEdgeSampler, UV, 0);
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
            LightSampleSrcLightRecord LightRecord = (LightSampleSrcLightRecord)0;
            LightRecord.Packed = RWVolumeRayToTraceSampledLightIndexBuffer[RayIndex];
            LightGrid_UpdateVisibilityForLightRecord(WorldPosition, RayToTrace.Direction, LightRecord);
        }
    }
}

/********************************************
 * Volume Grid Direct Lighting
 ********************************************/

// Volume Grid Header
StructuredBuffer<VolumeGridHeader> VolumeGridHeaderBuffer;

// Volume Grid Rays
RWStructuredBuffer<uint>   RWVolumeGridTransmittanceRayToTraceCount;
RWStructuredBuffer<float3> RWVolumeGridTransmittanceRayToTraceDirectionBuffer;
RWStructuredBuffer<uint>   RWVolumeGridTransmittanceRayToTraceStateBuffer;
RWStructuredBuffer<float3> RWVolumeGridTransmittanceRayToTraceOriginBuffer;
RWStructuredBuffer<float>  RWVolumeGridTransmittanceRayToTraceTMaxBuffer;
RWStructuredBuffer<uint>   RWVolumeGridTransmittanceRayToTraceSampledLightIndexBuffer;
RWStructuredBuffer<uint>   RWVolumeGridTransmittanceRayToTracePixelIndexBuffer;
RWStructuredBuffer<float>  RWVolumeGridTransmittanceRayToTraceTransmittanceBuffer;

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeGridRadianceEstimateTexture;

// Volume Grid Output Sum Transmittance
[[vk::image_format("r32f")]]
RWTexture2D<float> RWVolumeGridSumTransmittanceTexture;
// Volume Grid Direct Lighting Scatter Information(Color and Depth)
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeGridSampledColorAndDepth;
// Volume Grid Output Lighting
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeGridDirectLightingTexture;

[numthreads(1, 1, 1)]
void VolumeGridDirectLightingClearCounters() {
    RWVolumeGridTransmittanceRayToTraceCount[0] = 0;
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void VolumeGridDirectLightingSpawnLightSamples(uint2 GroupID : SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelIndex = GroupID * TILE_SIZE + LocalID;
    if (any(PixelIndex >= View.Camera.FilmDimensions)) return;

    // Initialize the output texture
    RWVolumeGridRadianceEstimateTexture[PixelIndex] = 0.f.xxxx;
    RWVolumeGridSampledColorAndDepth[PixelIndex] = 0.f.xxxx;
    RWVolumeGridDirectLightingTexture[PixelIndex] = 0.f.xxxx;

    CameraParameters C = GetActiveCamera();

    // 1. Generate camera rays
    float2 PixelUV = ScreenCoordsToUV(C, PixelIndex);
    float3 RayOrigin = C.Position;
    float3 RayDirection = normalize(NDC2ToCameraDirectionUnnormalized(C, UVToNDC2(PixelUV)));
    // Obtain depth for cropping
    float ReversedZDepth = G_DepthTexture.SampleLevel(PointEdgeSampler, PixelUV, 0).x;
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float TMax = LinearDepth / dot(RayDirection, C.Direction);

    // 2. Traverse VolumeGrid
    // TODO: Traverse all VolumeGrid
    uint VolumeGridIndex = 0;
    VolumeGridHeader Grid = VolumeGridHeaderBuffer[VolumeGridIndex];

    // 3. AABB
    float t0, t1;
    IntersectAABB(RayOrigin, RayDirection, Grid.LocalMin, Grid.LocalMax, t0, t1);
    t0 = max(t0, 0.f);
    t1 = min(t1, TMax);
    if(t0 >= t1) return;

    // 4. Delta Tracking
    Random rng = MakeRandom(PixelIndex.x + PixelIndex.y * C.FilmDimensions.x, 17491741 + DirectLighting_UB.FrameIndex);

    uint bindlessIndex = Grid.TextureBindlessIndex;
    Texture3D<float4> densityTex = GetBindlessVolumeSRV(bindlessIndex);

    float3 boxSize = Grid.LocalMax - Grid.LocalMin;
    float3 invBoxSize = 1.0f / boxSize;
    float3 uvwOrigin = (RayOrigin - Grid.LocalMin) * invBoxSize;
    float3 uvwDir = RayDirection * invBoxSize; // Non normalized

    // Using DDA to calculate majorant
    float densityScale = 1.0f;
    float majorant = CalculateMaxDensityDDA(densityTex, uvwOrigin, uvwDir, t0, t1, densityScale);
    majorant = max(majorant, 1e-6f);

    float t = t0;
    bool scattered = false;
    float3 scatterPos = 0.f.xxx;
    float3 volumeColor = 1.0f.xxx; // Albedo

    float accumulatedTransmittance = RWVolumeGridSumTransmittanceTexture[PixelIndex];

    // Delta Tracking Loop
    int loopLimit = 256;
    for(int i = 0; i < loopLimit; i++) {
        t -= log(1.0f - rng.rand()) / majorant;
        if (t >= t1) break;

        float3 pos = RayOrigin + RayDirection * t;
        float3 uvw = (pos - Grid.LocalMin) * invBoxSize;

        float4 sampleVal = densityTex.SampleLevel(LinearWrapSampler, uvw, 0);
        float density = sampleVal.a * densityScale;

        float nullProb = 1.0f - min(density, majorant) / majorant;
        accumulatedTransmittance *= nullProb;

        // Accept/Refuse
        if ((!scattered) && (rng.rand() < (density / majorant))) {
            scattered = true;
            scatterPos = pos;
            volumeColor = sampleVal.rgb;
            RWVolumeGridSampledColorAndDepth[PixelIndex] = float4(volumeColor, t);
        }

        // If transmittance is close to 0, break.
        if (accumulatedTransmittance < 0.001f) {
            accumulatedTransmittance = 0.0f;
            break;
        }
    }

    // Write Transmittance back
    RWVolumeGridSumTransmittanceTexture[PixelIndex] = accumulatedTransmittance;

    // 5. NEE
    float SumResampleWeights = 0.f;
    uint NumValidSamples = 0;
    float LightGridLightListCdf = 0;
    float3 RadianceEstimation = 0;

    LightSample ReservedSample = SampleOneLightSample_RIS(
        scatterPos, 0.f.xxx, -RayDirection,
        false, true, false, true,
        rng,
        RadianceEstimation,
        SumResampleWeights, NumValidSamples, LightGridLightListCdf
    );

    bool scatterValid = scattered && ReservedSample.IsValid() && (dot(RadianceEstimation, 1.f.xxx) > 0);

    float3 TraceDirection = ReservedSample.IsInfiniteLight() ? ReservedSample.Position : (ReservedSample.Position - scatterPos);
    float TraceDistance = ReservedSample.IsInfiniteLight() ? C.FarPlane : length(TraceDirection);
    TraceDirection /= max(TraceDistance, 1e-9);

    float3 FinalThroughput = RadianceEstimation * volumeColor;

    RWVolumeGridRadianceEstimateTexture[PixelIndex] = float4(FinalThroughput, 1.0f);

    // Transmittance Ray
    bool bPrimaryThread = WaveIsFirstLane();
    uint WaveRayCount = WaveActiveCountBits(scatterValid);
    uint WaveRayOffset = 0;
    if (bPrimaryThread) {
        InterlockedAdd(RWVolumeGridTransmittanceRayToTraceCount[0], WaveRayCount, WaveRayOffset);
    }
    WaveRayOffset = WaveReadLaneFirst(WaveRayOffset);
    uint WaveLocalRayOffset = WavePrefixCountBits(scatterValid);
    uint RayIndex = WaveRayOffset + WaveLocalRayOffset;

    if(scatterValid) {
        RWVolumeGridTransmittanceRayToTraceOriginBuffer[RayIndex] = scatterPos;
        RWVolumeGridTransmittanceRayToTraceDirectionBuffer[RayIndex] = TraceDirection;
        RWVolumeGridTransmittanceRayToTraceStateBuffer[RayIndex] = PackRayToTraceState(0.f, false);
        RWVolumeGridTransmittanceRayToTraceTMaxBuffer[RayIndex] = TraceDistance * DirectLighting_UB.ShadowRayLengthMultiplier;
        RWVolumeGridTransmittanceRayToTraceSampledLightIndexBuffer[RayIndex] = ReservedSample.LightRecord.Packed;

        RWVolumeGridTransmittanceRayToTracePixelIndexBuffer[RayIndex] = PackUint2x16(PixelIndex);
    }
}

// HWRT Calculating Transmittance Rays……

RayToTrace FetchVolumeGridRayToTraceWithWorldOrigin(uint RayIndex, float TMax) {
    RayToTrace Ray = (RayToTrace)0;
    Ray.Origin = RWVolumeGridTransmittanceRayToTraceOriginBuffer[RayIndex];
    Ray.Direction = RWVolumeGridTransmittanceRayToTraceDirectionBuffer[RayIndex];
    uint RayToTraceState = RWVolumeGridTransmittanceRayToTraceStateBuffer[RayIndex];
    Ray.TMax = TMax;
    Ray.TCurrent = UnpackRayToTraceState(RayToTraceState, Ray.bHit);
    return Ray;
}

[numthreads(WAVE_SIZE, 1, 1)]
void RenderVolumeGridDirectLighting(uint DispatchThreadID : SV_DispatchThreadID)
{
    uint RayIndex = DispatchThreadID;
    if(RayIndex >= RWVolumeGridTransmittanceRayToTraceCount[0]) return;

    RayToTrace RayToTrace = FetchVolumeGridRayToTraceWithWorldOrigin(RayIndex, 0);
    uint2 PixelIndex = UnpackUint2x16(RWVolumeGridTransmittanceRayToTracePixelIndexBuffer[RayIndex]);

    float RayTransmittance = RWVolumeGridTransmittanceRayToTraceTransmittanceBuffer[RayIndex];

    float3 Estimate = RWVolumeGridRadianceEstimateTexture[PixelIndex].rgb;
    float3 FinalRadiance = Estimate * RayTransmittance;

    RWVolumeGridDirectLightingTexture[PixelIndex] = float4(FinalRadiance, 1.f);
}
