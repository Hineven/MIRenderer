#include "shared/SharedDebug.hlsl"
#include "shared/SharedVolumePrimitives.hlsl"
#include "headers/Conventions.hlsl"
#include "headers/Camera.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Random.hlsl"
#include "headers/OctahedronMapping.hlsl"
#include "headers/Math.hlsl"
#include "headers/RadiometryAndColorSpace.hlsl"
#include "headers/Sampling.hlsl"
#include "headers/SphericalHarmonics.hlsl"
#include "headers/HybridTracing.hlsl"
#include "headers/MaterialEvaluation.hlsl"
#include "headers/CommonIndirectLighting.hlsl"
#include "headers/VolumeScattering.hlsl"
#include "resources/HashGridCacheResources.hlsl"
#include "resources/CommonSamplerResources.hlsl"
#include "resources/LightGridSampling.hlsl"

// Volume probes
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeProbeRadianceDepthTexture;
// Volume probe header
[[vk::image_format("rgba32ui")]]
RWTexture2D<uint4>  RWVolumeProbeHeaderTexture;

// SH projection of foreground probes
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeProbeIrradianceTexture; // UB.TileDimensions
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeProbeSHCoefficientsRTexture; // Doubled width ( to store 4 + 4 floats )
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeProbeSHCoefficientsGTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeProbeSHCoefficientsBTexture;

RWStructuredBuffer<uint4> RWSpawnedVolumeProbeHeaderBuffer; // Used to keep new volume probe headers spawned this frame
// Atlas for newly spawned probes
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeProbeReconstructedRadianceDepthTexture;
RWStructuredBuffer<uint> RWVolumeProbeSpawnAllocator; // Spawn allocator for volume probes
RWStructuredBuffer<uint> RWVolumeProbeMRUQueueBuffer; // A MRU queue of volume probe indices (overwrite LRU elements first when spawnning)
RWStructuredBuffer<uint> RWVolumeProbeNextMRUQueueBuffer; // Used to build the MRU queue for next frame

struct VolumeProbeHeader {
    float3 WorldPosition;
    bool bActive;
    bool bTemporalBlendable;
};

VolumeProbeHeader UnpackVolumeProbeHeader (uint4 Packed) {
    VolumeProbeHeader Header;
    Header.WorldPosition = asfloat(Packed.xyz);
    Header.bActive = Packed.w & 0x1u;
    Header.bTemporalBlendable = Packed.w & 0x2u;
    return Header;
}

uint4 PackVolumeProbeHeader (VolumeProbeHeader Header) {
    uint4 Packed;
    Packed.xyz = asuint(Header.WorldPosition);
    Packed.w = 0;
    Packed.w |= (Header.bActive ? 0x1u : 0x0u);
    Packed.w |= (Header.bTemporalBlendable ? 0x2u : 0x0u);
    return Packed;
}

uint4 ProbeHeaderMarkTemporalBlendable (uint4 Packed) {
    Packed.w |= 0x2;
    return Packed;
}

// Active volume probes in the previous frame
StructuredBuffer<uint> PreviousActiveVolumeProbeCount;
StructuredBuffer<uint> PreviousActiveVolumeProbeListBuffer;

// All active volume probe atlas indices.
RWStructuredBuffer<uint> RWActiveVolumeProbeCount;
RWStructuredBuffer<uint> RWActiveVolumeProbeListBuffer;

// Indexing structure: per-tile lists of volume probe atlas indices
RWStructuredBuffer<uint>  RWTileVolumeProbeIndexListBuffer;
RWStructuredBuffer<uint>  RWTileVolumeProbeIndexListLengthsBuffer;
RWStructuredBuffer<uint>  RWTileVolumeProbeIndexListOffsetsBuffer;
RWStructuredBuffer<uint>  RWTileVolumeProbeIndexListAllocator; // Used to allocate RWTileVolumeProbeIndexListBuffer entries to tiles
// Used for volume probe reprojection
RWStructuredBuffer<uint>  RWTileVolumeProbeReprojectionEntryAllocator;
RWStructuredBuffer<uint3> RWTileVolumeProbeReprojectionEntryBuffer;

RWStructuredBuffer<uint>  RWTileSpawnedVolumeProbeBuffer; // Post-injection for per-tile newly spawned probes this frame

// Spawned rays
RWStructuredBuffer<uint>   RWVolumeProbeUpdateRayOffsetsBuffer;
RWStructuredBuffer<uint>   RWVolumeProbeUpdateRayCountsBuffer;
RWStructuredBuffer<float3> RWVolumeProbeUpdateRayDirectionBuffer;
RWStructuredBuffer<uint>   RWVolumeProbeUpdateRayStateBuffer;
RWStructuredBuffer<float3> RWVolumeProbeUpdateRayOriginBuffer;
RWStructuredBuffer<uint>   RWVolumeProbeUpdateRayAllocator; // Number of all rays to be traced

// Ray trace results
RWStructuredBuffer<uint2> RWVolumeProbeUpdateRayResultBuffer; // Packed CachedHitMaterial
RWStructuredBuffer<uint2> RWVolumeProbeUpdateRayRadianceBuffer; // Fp16x4 packed radiance + flag
RWStructuredBuffer<float> RWVolumeProbeUpdateRayInvPdfBuffer;
RWStructuredBuffer<uint>  RWVolumeProbeUpdateRayHitResolveBucketAndCellOffsetBuffer;

// Shading counters for update rays
RWStructuredBuffer<uint>  RWVolumeProbeUpdateRayHitShadingPointAllocator;
RWStructuredBuffer<uint>  RWVolumeProbeUpdateRayHitShadingPointListBuffer; // Shading point -> update ray index

// Transmittance ray traces
RWStructuredBuffer<uint>   RWShadePointTransmittanceRayAllocator;
RWStructuredBuffer<float3> RWShadePointTransmittanceRayDirectionBuffer;
RWStructuredBuffer<float3> RWShadePointTransmittanceRayOriginBuffer;
RWStructuredBuffer<uint>   RWShadePointTransmittanceRayStateBuffer;
RWStructuredBuffer<float>  RWShadePointTransmittanceRayTMaxBuffer;
StructuredBuffer<float>    ShadePointTransmittanceRayTransmittanceBuffer;

RWStructuredBuffer<uint>   RWShadePointTransmittanceRaySampledLightIndexBuffer;

RWStructuredBuffer<uint2>  RWShadePointTransmittanceRayContributionBuffer;
RWStructuredBuffer<uint>   RWShadePointToTransmittanceRayIndexBuffer;

Texture2D<float4> PreviousNormalTexture;
Texture2D<float>  PreviousDepthTexture;
Texture2D<float>  PreviousTransmittanceTexture;
Texture2D<float4> PreviousVolumeColorTexture;
Texture2D<float4> PreviousShadedDiffuseRadianceWithoutEmission;

Texture2D<float>  G_VolumeSampleDepth;
Texture2D<float4> G_VolumeSampleColor;
Texture2D<float2> VolumeMinMaxTexture;
Texture2D<float>  VolumeDensityTexture;

Texture2D<float2> PreviousVolumeMinMaxTexture;
Texture2D<float>  PreviousVolumeDensityTexture;
Texture2D<float4> PreviousVolumeRadianceTexture;

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWVolumeIndirectLightingTexture;

struct VolumeDiffuseIndirectLightingUB {
    uint  MaxNumUpdateRays; // Must be a multiple of WAVE_SIZE
    float GRF_EmitterIntensityScale;
    uint2 TileDimensions;

    float2 InvTileDimensions;
    uint  TileCount;
    uint  ResetCache;

    float  ProbeReprojectionSearchSize;
    uint   MaxProbesToSpawnPerFrame;
    float2 InvProbeAtlasDimensions;
    
    uint FrameIndex;
    uint ProbeUpdateRaysNoImportanceSampling;
    uint ScreenReuseNoDepthTesting;
    uint ProbeUpdateRaySampleSeed;

    uint ProbeUpdateRaysNoAdaptiveAllocation;
    uint ProbeSpawnSubTileJitterSeed;
    uint TileProbeSpawnSeed;
    uint NoEnvironmentLight; // For debugging

    uint NoIndirectLighting;
    float LnProbeDepthSearchTransmittanceThresh;
    uint NoScreenReuseEnergyDecay;
    uint Padding0;
};

ConstantBuffer<VolumeDiffuseIndirectLightingUB> UB;

[numthreads(1, 1, 1)]
void ClearCounters () {
    RWVolumeProbeSpawnAllocator[0] = 0;
    RWActiveVolumeProbeCount[0] = 0;
    RWTileVolumeProbeIndexListAllocator[0] = 0;
    RWTileVolumeProbeReprojectionEntryAllocator[0] = 0;
    RWVolumeProbeUpdateRayAllocator[0] = 0;
    // RWTileScreenProbeCacheIndexListAllocator[0] = 0;
    RWVolumeProbeUpdateRayHitShadingPointAllocator[0] = 0;
    RWShadePointTransmittanceRayAllocator[0] = 0;
}

// Clear per-tile list lengths and offsets to avoid reading garbage from previous frames
[numthreads(WAVE_SIZE, 1, 1)]
void ClearTileVolumeProbeIndexLists (uint DispatchID : SV_DispatchThreadID) {
    uint TileIndex1 = DispatchID;
    if (TileIndex1 >= UB.TileCount) return;
    RWTileVolumeProbeIndexListLengthsBuffer[TileIndex1] = 0;
    RWTileVolumeProbeIndexListOffsetsBuffer[TileIndex1] = 0;
    RWTileSpawnedVolumeProbeBuffer[TileIndex1] = INVALID_UINT;
}

// Called when application requests a history reset. Drop all history probes
// and re-initialize the MRU queue.
[numthreads(WAVE_SIZE, 1, 1)]
void InitializeVolumeProbeCache (uint DispatchID : SV_DispatchThreadID) {
    uint ProbeIndex1 = DispatchID;
    if(ProbeIndex1 >= UB.TileCount) return;
    // Initialize the MRU queue.
    RWVolumeProbeMRUQueueBuffer[ProbeIndex1] = ProbeIndex1;
    // De-activate all probes
    uint2 ProbeIndex = uint2(ProbeIndex1 % UB.TileDimensions.x, ProbeIndex1 / UB.TileDimensions.x);
    RWVolumeProbeHeaderTexture[ProbeIndex] = PackVolumeProbeHeader((VolumeProbeHeader)0);
}

// Inject volume probes from last frame to the index of this frame
[numthreads(WAVE_SIZE, 1, 1)]
void InjectVolumeProbes (uint DispatchID : SV_DispatchThreadID) {
    uint PreviousProbeIndex1 = DispatchID;
    if(PreviousProbeIndex1 >= UB.TileCount) return;
    uint2 PreviousProbeIndex = uint2(PreviousProbeIndex1 % UB.TileDimensions.x, PreviousProbeIndex1 / UB.TileDimensions.x);
    VolumeProbeHeader Header = UnpackVolumeProbeHeader(RWVolumeProbeHeaderTexture[PreviousProbeIndex]);
    CameraParameters C = GetActiveCamera();
    if(Header.bActive) {
        uint CurrentProbeActiveListIndex;
        InterlockedAdd(RWActiveVolumeProbeCount[0], 1, CurrentProbeActiveListIndex);
        RWActiveVolumeProbeListBuffer[CurrentProbeActiveListIndex] = PreviousProbeIndex1;

        // Inject to the tile's volume probe index list
        float3 NDC = TransformPoint(C.WorldToNDC, Header.WorldPosition);
        float2 UV  = NDC2ToUV(NDC.xy);
        if (all(UV > 0.0f) && all(UV < 1.0f))
        {
            uint2 TileIndex = floor(UV * UB.TileDimensions);
            uint TileIndex1 = TileIndex.x + TileIndex.y * UB.TileDimensions.x;
            uint TileEntryIndex;
            InterlockedAdd(RWTileVolumeProbeIndexListLengthsBuffer[TileIndex1], 1, TileEntryIndex);

            uint GlobalEntryIndex;
            InterlockedAdd(RWTileVolumeProbeReprojectionEntryAllocator[0], 1, GlobalEntryIndex);
            // Store atlas index (PreviousProbeIndex1), not compact active-list index, in the tile list entry
            RWTileVolumeProbeReprojectionEntryBuffer[GlobalEntryIndex] = uint3(TileIndex1, TileEntryIndex, PreviousProbeIndex1);
        }
    }
}

[numthreads(WAVE_SIZE, 1, 1)]
void AllocateTileVolumeProbeLists (uint DispatchID : SV_DispatchThreadID) {
    uint TileIndex1 = DispatchID;
    if(TileIndex1 >= UB.TileCount) return;

    uint TileEntryBase;
    InterlockedAdd(RWTileVolumeProbeIndexListAllocator[0], RWTileVolumeProbeIndexListLengthsBuffer[TileIndex1], TileEntryBase);
    RWTileVolumeProbeIndexListOffsetsBuffer[TileIndex1] = TileEntryBase;
}

[numthreads(WAVE_SIZE, 1, 1)]
void ScatterReprojectedVolumeProbesToTileList (uint DispatchID : SV_DispatchThreadID) {
    uint GlobalEntryIndex = DispatchID;
    if(GlobalEntryIndex >= RWTileVolumeProbeReprojectionEntryAllocator[0]) return;
    uint3 ReprojectionEntry = RWTileVolumeProbeReprojectionEntryBuffer[GlobalEntryIndex];
    uint TileIndex1       = ReprojectionEntry.x;
    uint TileEntryIndex   = ReprojectionEntry.y;
    uint VolumeProbeIndex = ReprojectionEntry.z;
    uint TileEntryBase = RWTileVolumeProbeIndexListOffsetsBuffer[TileIndex1];
    RWTileVolumeProbeIndexListBuffer[TileEntryBase + TileEntryIndex] = VolumeProbeIndex;
}

bool ShouldSpawnProbe (uint2 TileIndex) {
    // Simple uniform spawnning pattern: one probe per tile
    return true;
}

uint2 GetProbeSpawnSubTileJitter () {
    return min(
        uint2(CalculateHaltonSequence(UB.ProbeSpawnSubTileJitterSeed) * TILE_SIZE),
        TILE_SIZE - 1
    );
}

// Spawn volume probes
[numthreads(WAVE_SIZE, 1, 1)]
void SpawnVolumeProbes (uint DispatchID : SV_DispatchThreadID) {
    uint TileIndex1 = DispatchID;
    if(TileIndex1 >= UB.TileCount) return;
    uint2 TileIndex = uint2(TileIndex1 % UB.TileDimensions.x, TileIndex1 / UB.TileDimensions.x);

    // Spawn one probe per tile. This can be adjusted to spawn interleaved probes.
    if(ShouldSpawnProbe(TileIndex)) {
        CameraParameters C  = GetActiveCamera();
        uint2  SpawnSubTileJitter = GetProbeSpawnSubTileJitter();
        uint2  SpawnScreenCoords  = min(TileIndex * TILE_SIZE + SpawnSubTileJitter, C.FilmDimensions - 1);
        float2 SpawnUV = SpawnScreenCoords * C.InvFilmDimensions;
        float  SpawnLinearDepth   = G_VolumeSampleDepth.SampleLevel(PointEdgeSampler, SpawnUV, 0).x;
    
        if (SpawnLinearDepth == 0) {
            // Failed, fallback to spawnning via ray volume statistics
            float2 VolumeMinMax  = VolumeMinMaxTexture.SampleLevel(PointEdgeSampler, SpawnUV, 0).xy;
            if(VolumeMinMax.y > VolumeMinMax.x) {
                float  VolumeDensity = VolumeDensityTexture.SampleLevel(PointEdgeSampler, SpawnUV, 0).r;
                float  Transmittance = IntegrateExponentialScatteringMedium(VolumeDensity, VolumeMinMax.y - VolumeMinMax.x);
                float  U = MakeRandom(TileIndex1, 53719371u + UB.FrameIndex).rand();
                // Modify the random number, making sure that we don't sample beyond the volume bounds
                float  FlyDist = SampleExponentialScatteringMedium(VolumeDensity, U * (1.f - Transmittance));
                if(FlyDist + VolumeMinMax.x < VolumeMinMax.y) { // Safety check
                    float RayLengthModifier = 1.f / length(NDC2ToCameraDirectionUnnormalized(C, UVToNDC2(SpawnUV)));
                    SpawnLinearDepth = (FlyDist + VolumeMinMax.x) * RayLengthModifier;
                }
            }
        }

        if (SpawnLinearDepth > 0)
        {
            uint AllocatedWaveRank = 0;
            AllocatedWaveRank = WavePrefixCountBits(true);
            uint AllocatedWaveSum = WaveActiveCountBits(true);
            uint AllocatedWaveIndexBase = 0;
            if(WaveIsFirstLane()) {
                InterlockedAdd(
                    RWVolumeProbeSpawnAllocator[0],
                    AllocatedWaveSum, AllocatedWaveIndexBase
                );
            }
            AllocatedWaveIndexBase = WaveReadLaneFirst(AllocatedWaveIndexBase);
            uint AllocatedIndex = AllocatedWaveIndexBase + AllocatedWaveRank;
            if(AllocatedIndex < UB.MaxProbesToSpawnPerFrame) {
                // Write header data
                VolumeProbeHeader Header = (VolumeProbeHeader)0;
                Header.WorldPosition = RecoverWorldPositionPixelCoords(C, SpawnScreenCoords, SpawnLinearDepth);
                Header.bActive = true;
                RWSpawnedVolumeProbeHeaderBuffer[AllocatedIndex] = PackVolumeProbeHeader(Header);
            }
        }
    }
}

[numthreads(1, 1, 1)]
void ClipVolumeProbeSpawnAllocator () {
    RWVolumeProbeSpawnAllocator[0] = min(
        RWVolumeProbeSpawnAllocator[0],
        UB.MaxProbesToSpawnPerFrame
    );
}

float GetProbeDepthSearchSizeForDensity (float Density) {
    return -UB.LnProbeDepthSearchTransmittanceThresh / max(Density, 0.2f); 
}

float RadianceToSampleWeight (float3 Radiance) {
    return RadianceToLuminance(Radiance) + 0.001f; // Avoid zero weight
}

bool TestProbeOcclusion (float2 ProbeAScreenPos, float ProbeALinearDepth, float ProbeASurfaceLinearDepth,
                         float2 ProbeBScreenPos, float ProbeBLinearDepth, float ProbeBSurfaceLinearDepth,
                         float ProbePixelSearchSize, float VolumeDepthProbeSearchSize, float SurfaceDepthProbeSearchSize) {
    
    float ProbeARelLinearDepth = max(0, ProbeALinearDepth - ProbeASurfaceLinearDepth);
    float ProbeBRelLinearDepth = max(0, ProbeBLinearDepth - ProbeBSurfaceLinearDepth);

    bool bProbeScreenTest = length(ProbeAScreenPos - ProbeBScreenPos) < ProbePixelSearchSize;
    bool bProbeDepthTestA = abs(ProbeALinearDepth - ProbeBLinearDepth) < VolumeDepthProbeSearchSize;
    // Testing with depth relative to first volume surface. This works well when the camera is far away from the volume
    bool bProbeDepthTestB = abs(ProbeARelLinearDepth - ProbeBRelLinearDepth) < VolumeDepthProbeSearchSize;
    // To make volume depth test B pass, the surfaces between the closest volumes should also be similar
    bool bSurfaceDepthTest = abs(ProbeASurfaceLinearDepth - ProbeBSurfaceLinearDepth) < SurfaceDepthProbeSearchSize;

    return bProbeScreenTest && (bProbeDepthTestA || (bProbeDepthTestB && bSurfaceDepthTest));
}

#define MAX_NUM_UPDATE_RAYS_PER_PROBE  (2 * TILE_TEXEL_COUNT)

groupshared uint4 SharedProbeBlendedRadiance[TILE_TEXEL_COUNT];
groupshared uint  SharedProbeTexelWeight[TILE_TEXEL_COUNT];
groupshared float SharedProbeOctahedronSampleWeight[TILE_TEXEL_COUNT];
groupshared float SharedProbeOctahedronSampleWeightPrefixSum[TILE_TEXEL_COUNT];

[numthreads(WAVE_SIZE, 1, 1)]
void ReconstructRadiance_SampleSpawnVolumeProbeUpdateRays (uint GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint SpawnListIndex = GroupID;
    uint SpawnListCount = min(
        RWVolumeProbeSpawnAllocator[0],
        UB.MaxProbesToSpawnPerFrame
    );
    if(SpawnListIndex >= SpawnListCount) return;

    CameraParameters C = GetActiveCamera();
    VolumeProbeHeader Header = UnpackVolumeProbeHeader(RWSpawnedVolumeProbeHeaderBuffer[SpawnListIndex]);

    float3 ProbeNDC  = TransformPoint(C.WorldToNDC, Header.WorldPosition);
    float2 ProbeUV   = NDC2ToUV(ProbeNDC.xy);
    float2 ProbeScreenPos = ProbeUV * C.FilmDimensions;
    uint2  TileIndex = ProbeScreenPos / TILE_SIZE;
    
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex1 = BaseTexelIndex + LocalID;
        SharedProbeBlendedRadiance[TexelIndex1] = 0;
        SharedProbeTexelWeight[TexelIndex1] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    
    float ProbeLinearDepth = dot(Header.WorldPosition - C.Position, C.Direction);
    float ProbePixelSearchSize = UB.ProbeReprojectionSearchSize;
    float SurfaceDepthProbeSearchSize = ProbePixelSearchSize * max(C.FilmPixelWorldSize.x, C.FilmPixelWorldSize.y) * ProbeLinearDepth;
    // Along the camera's z-axis, the search size solely depends on volume density
    float ProbeVolumeDensity = VolumeDensityTexture.SampleLevel(PointEdgeSampler, ProbeUV, 0).r;
    float VolumeDepthProbeSearchSize = GetProbeDepthSearchSizeForDensity(ProbeVolumeDensity);

    float ProbeToLinear = dot(normalize(Header.WorldPosition - C.Position), C.Direction);
    float ProbeSurfaceLinearDepth = VolumeMinMaxTexture.SampleLevel(PointEdgeSampler, ProbeUV, 0).x * ProbeToLinear;
    float ProbeRelLinearDepth = max(ProbeLinearDepth - ProbeSurfaceLinearDepth, 0);

    float3 SumWeightedRadiance = 0;
    float  SumReusedWeight = 0;

    // // Try to recover radiance at the new probe from probes in neighboring tiles
    for(int dx = -1; dx <= 1; dx++) {
        for(int dy = -1; dy <= 1; dy++) {
            int2 NeighborTileIndex = int2(TileIndex) + int2(dx, dy);
            if(any(NeighborTileIndex < 0) || any(NeighborTileIndex >= int2(UB.TileDimensions))) continue ;
            uint NeighborTileIndex1 = NeighborTileIndex.x + NeighborTileIndex.y * UB.TileDimensions.x;
            uint TileInjectedListLength = RWTileVolumeProbeIndexListLengthsBuffer[NeighborTileIndex1];
            uint TileInjectedListOffset = RWTileVolumeProbeIndexListOffsetsBuffer[NeighborTileIndex1];
            for(uint ProbeMRUListRank = 0; ProbeMRUListRank < TileInjectedListLength; ProbeMRUListRank++) {
                uint TileIndexListIndex = TileInjectedListOffset + ProbeMRUListRank;
                uint InjectedVolumeProbeIndex1 = RWTileVolumeProbeIndexListBuffer[TileIndexListIndex];
                uint2 InjectedVolumeProbeIndex = uint2(InjectedVolumeProbeIndex1 % UB.TileDimensions.x, InjectedVolumeProbeIndex1 / UB.TileDimensions.x);
                VolumeProbeHeader InjectedProbeHeader = UnpackVolumeProbeHeader(RWVolumeProbeHeaderTexture[InjectedVolumeProbeIndex]);
                float3 InjectedProbeNDC = TransformPoint(C.WorldToNDC, InjectedProbeHeader.WorldPosition);
                float2 InjectedProbeUV  = NDC2ToUV(InjectedProbeNDC.xy);
                float2 InjectedProbeScreenPos = InjectedProbeUV * C.FilmDimensions;
                float  InjectedProbeLinearDepth = dot(InjectedProbeHeader.WorldPosition - C.Position, C.Direction);
                float  InjectedProbeToLinear = dot(normalize(InjectedProbeHeader.WorldPosition - C.Position), C.Direction);
                float  InjectedProbeSurfaceLinearDepth = VolumeMinMaxTexture.SampleLevel(PointEdgeSampler, InjectedProbeUV, 0).x * InjectedProbeToLinear;
                float  InjectedProbeRelLinearDepth = max(InjectedProbeLinearDepth - InjectedProbeSurfaceLinearDepth, 0);

                if(TestProbeOcclusion(
                    ProbeScreenPos, ProbeLinearDepth, ProbeSurfaceLinearDepth,
                    InjectedProbeScreenPos, InjectedProbeLinearDepth, InjectedProbeSurfaceLinearDepth,
                    ProbePixelSearchSize, VolumeDepthProbeSearchSize, SurfaceDepthProbeSearchSize)) {
                    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
                        uint TexelIndex1 = BaseTexelIndex + LocalID;
                        uint2 ProbeTexelCoords = int2(TexelIndex1 % TILE_SIZE, TexelIndex1 / TILE_SIZE);
                        uint2 VolumeProbeAtlasTexelCoords = InjectedVolumeProbeIndex * TILE_SIZE + ProbeTexelCoords;
                        float4 ProbeRadianceDepth = RWVolumeProbeRadianceDepthTexture[VolumeProbeAtlasTexelCoords];
                        float2 ProbeTexelUV = (ProbeTexelCoords + 0.5f) / TILE_SIZE;
                        // For volume probes, there're no local frame
                        float3 ProbeDirection = Octahedron01ToUnitVector(ProbeTexelUV);
                        float3 HitPosition = InjectedProbeHeader.WorldPosition + ProbeDirection * ProbeRadianceDepth.w;
                        float3 ReprojectedDirection = HitPosition - Header.WorldPosition;
                        float  ReprojectedDepth = length(ReprojectedDirection);
                        ReprojectedDirection /= ReprojectedDepth;
                        {
                            float2 ReprojectedTexelUV = UnitVectorToOctahedron01(ReprojectedDirection);
                            uint2  ReprojectedTexelCoords = uint2(ReprojectedTexelUV * TILE_SIZE);
                            uint   ReprojectedTexelIndex = ReprojectedTexelCoords.x + ReprojectedTexelCoords.y * TILE_SIZE;
                            // Use differential to weight the contribution for unbiased approximation
                            // (Note: not InvPdf, because we're just averaging the contributions per texel here)
                            float  Weight = dSphericalAngle_dOctahedronArea01(ReprojectedDirection);
                            // TODO texel reprojection
                            float  TexelReprojectionWeight = 1.f;
                            Weight *= TexelReprojectionWeight;
                            float3 WeightedProbeRadianceDepth = ProbeRadianceDepth.xyz * Weight;
                            InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].x, QuantilizeRadiance(WeightedProbeRadianceDepth.x));
                            InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].y, QuantilizeRadiance(WeightedProbeRadianceDepth.y));
                            InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].z, QuantilizeRadiance(WeightedProbeRadianceDepth.z));
                            InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].w, QuantilizeRadiance(Weight * ReprojectedDepth));
                            InterlockedAdd(SharedProbeTexelWeight[ReprojectedTexelIndex], QuantilizeWeight(Weight));
                            SumWeightedRadiance += ProbeRadianceDepth.xyz * Weight;
                            SumReusedWeight += Weight;
                        }
                    }
                }
            }
        }
    }
    
    GroupMemoryBarrierWithGroupSync();
    SumWeightedRadiance = WaveActiveSum(SumWeightedRadiance);
    SumReusedWeight = WaveActiveSum(SumReusedWeight);
    float3 BackupRadiance = SumWeightedRadiance / max(1e-5f, SumReusedWeight);

    // Sample rays
    // We assume that ray count is always a multiple of WAVE_SIZE

    const float Epsilon = 1e-6f;

    float SumSizeOctahedronOriginal = 0.f;
    // Resolve octahedron sample weights
    {
        float SavedPrefixSum = 0.f;
        for(uint BaseProbeTexelIndex = 0; BaseProbeTexelIndex < TILE_TEXEL_COUNT; BaseProbeTexelIndex += WAVE_SIZE) {
            uint  ProbeTexelIndex = BaseProbeTexelIndex + LocalID;
            uint2 ProbeTexelCoords = uint2(ProbeTexelIndex % TILE_SIZE, ProbeTexelIndex / TILE_SIZE);
            float4 RadianceDepthSum = RecoverRadiance(uint4(SharedProbeBlendedRadiance[ProbeTexelIndex].x,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].y,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].z,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].w));
            float TexelSampleWeightSum = RecoverWeight(SharedProbeTexelWeight[ProbeTexelIndex]);
            // Write out reconstructed radiance to a separate buffer for later blending
            float4 ReconstructedRadianceDepth = (TexelSampleWeightSum > 0) ? (RadianceDepthSum / TexelSampleWeightSum) : float4(BackupRadiance, 1000);
            RWVolumeProbeReconstructedRadianceDepthTexture[TileIndex * TILE_SIZE + ProbeTexelCoords] = ReconstructedRadianceDepth;
            float3 Radiance = ReconstructedRadianceDepth.xyz;
            float3 RepresentativeDirection = Octahedron01ToUnitVector((float2(ProbeTexelCoords) + 0.5f) / TILE_SIZE);
            // Use a representative direction approximating the differential for better weighted sampling
            float AreaCorrectionFactor = dSphericalAngle_dOctahedronArea01(RepresentativeDirection);
            float  SampleWeight = RadianceToSampleWeight(Radiance) * AreaCorrectionFactor;
            SharedProbeOctahedronSampleWeight[ProbeTexelIndex] = SampleWeight;
            float PrefixSum = WavePrefixSum(SampleWeight) + SavedPrefixSum;
            SharedProbeOctahedronSampleWeightPrefixSum[ProbeTexelIndex] = PrefixSum;
#if TILE_TEXEL_COUNT % WAVE_SIZE != 0
#error "TILE_TEXEL_COUNT must be a multiple of WAVE_SIZE"
#endif
            SavedPrefixSum = WaveReadLaneAt(PrefixSum + SampleWeight, WAVE_SIZE - 1); 
        }
        SumSizeOctahedronOriginal = SavedPrefixSum;
    }
    GroupMemoryBarrierWithGroupSync();

    uint TileIndex1 = TileIndex.x + TileIndex.y * UB.TileDimensions.x;
    Random rng = MakeRandom((TileIndex1 * WAVE_SIZE + LocalID) ^ 0xa519c6adu, UB.ProbeUpdateRaySampleSeed);
#if MAX_NUM_UPDATE_RAYS_PER_PROBE % WAVE_SIZE != 0
#error "MAX_NUM_UPDATE_RAYS_PER_PROBE must be a multiple of WAVE_SIZE"
#endif
    // Allocate a number of rays to sample the probe octahedron
    int NumProbeOctahedronSamples = TILE_TEXEL_COUNT;
    NumProbeOctahedronSamples = min(NumProbeOctahedronSamples, MAX_NUM_UPDATE_RAYS_PER_PROBE);
    uint UpdateRayIndexBase = 0;
    if(WaveIsFirstLane()) {
        InterlockedAdd(RWVolumeProbeUpdateRayAllocator[0], NumProbeOctahedronSamples, UpdateRayIndexBase);
        uint RemainingRayCount = (UB.MaxNumUpdateRays <= UpdateRayIndexBase) 
            ? 0 : UB.MaxNumUpdateRays - UpdateRayIndexBase;
        NumProbeOctahedronSamples = min(NumProbeOctahedronSamples, RemainingRayCount);
        RWVolumeProbeUpdateRayCountsBuffer[SpawnListIndex]  = NumProbeOctahedronSamples;
        RWVolumeProbeUpdateRayOffsetsBuffer[SpawnListIndex] = UpdateRayIndexBase;
    }
    NumProbeOctahedronSamples = WaveReadLaneFirst(NumProbeOctahedronSamples);
    UpdateRayIndexBase = WaveReadLaneFirst(UpdateRayIndexBase);

    // If the probe has adequate samples from radiance reconstruction, mark it as temporal blendable
    if(WaveIsFirstLane()) {
        if(SumReusedWeight >= 32.f * FOUR_PI) {
            RWSpawnedVolumeProbeHeaderBuffer[SpawnListIndex] = 
                ProbeHeaderMarkTemporalBlendable(RWSpawnedVolumeProbeHeaderBuffer[SpawnListIndex]); // Mark as temporal blendable
        }
    }

    // Sample probe octahedron
    [unroll(MAX_NUM_UPDATE_RAYS_PER_PROBE / WAVE_SIZE)]
    for(uint RayRankBase = 0; RayRankBase < NumProbeOctahedronSamples; RayRankBase += WAVE_SIZE) {
        // We assume that ray count is always a multiple of WAVE_SIZE
        uint RayRank = RayRankBase + LocalID;
        float  u    = rng.rand();
        float2 u2   = rng.rand2();
        float  U    = u * SumSizeOctahedronOriginal;
        uint L = 0, R = TILE_TEXEL_COUNT;
        for(uint i = 0; i < TILE_TEXEL_COUNT_L2; i++) {
            uint Mid = (L + R) / 2;
            if(SharedProbeOctahedronSampleWeightPrefixSum[Mid] <= U) L = Mid;
            else R = Mid;
        }
        float OctPdf = SharedProbeOctahedronSampleWeight[L] / max(SumSizeOctahedronOriginal, Epsilon) * TILE_TEXEL_COUNT;
        float2 TexelInnerUV = u2;
        uint2   TexelCoords = int2(L % TILE_SIZE, L / TILE_SIZE);
        float2 OctahedronUV = (TexelInnerUV + TexelCoords) * (1.f / TILE_SIZE);
        float3 RayDirection = Octahedron01ToUnitVector(OctahedronUV);
        // Convert from [0, 1]^2 to S^2
        float PdfAreaCorrectionFactor = 1.f / dSphericalAngle_dOctahedronArea01(RayDirection);
        OctPdf = OctPdf * PdfAreaCorrectionFactor;

        float RayPdf = OctPdf;
        if(UB.ProbeUpdateRaysNoImportanceSampling) {
            RayPdf = SampleSphereUniformPdf();
            RayDirection = SampleSphereUniform(u2);
        }
        uint RayIndex = RayRank + UpdateRayIndexBase;
		// Setup ray to trace indirection list (for later compound tracing)
#define MIN_PDF_TO_TRACE 4e-3f
		if(RayPdf >= MIN_PDF_TO_TRACE) {
			// A valid update ray is spawned
			// Queue up for a ray trace
            RWVolumeProbeUpdateRayDirectionBuffer[RayIndex] = RayDirection;
            RWVolumeProbeUpdateRayStateBuffer[RayIndex] = 0; // Initial state
            RWVolumeProbeUpdateRayOriginBuffer[RayIndex] = Header.WorldPosition;
			RWVolumeProbeUpdateRayResultBuffer[RayIndex] = 0;
			// Keep extra data for later probe update
			float RayInvPdf = 1.f / RayPdf;
            RWVolumeProbeUpdateRayInvPdfBuffer[RayIndex] = RayInvPdf;
        } else {
            // Invalid update ray, mark it as invalid to skip tracing
            RWVolumeProbeUpdateRayDirectionBuffer[RayIndex] = 0.f.xxx;
            RWVolumeProbeUpdateRayStateBuffer[RayIndex] = 0; // Initial state
            RWVolumeProbeUpdateRayOriginBuffer[RayIndex] = 0;
            RWVolumeProbeUpdateRayResultBuffer[RayIndex] = 0;
            RWVolumeProbeUpdateRayInvPdfBuffer[RayIndex] = 0.f;
        }
    }
}

[numthreads(WAVE_SIZE, 1, 1)]
void ClipUpdateRayCount () {
    RWVolumeProbeUpdateRayAllocator[0] = min(RWVolumeProbeUpdateRayAllocator[0], UB.MaxNumUpdateRays);
}

// The sampled rays are traced in separate shaders via hybrid tracing (no written here)
// HWRT trace visibility rays (without indirection ray index list)

uint2 PackUpdateRayRadianceFlag (float3 Radiance, bool bBypass) {
    return PackFp16x4Safe(float4(Radiance, bBypass));
}

float3 UnpackUpdateRayRadianceFlag (uint2 Packed, out bool bBypass) {
    float4 v = UnpackFp16x4Safe(Packed);
    bBypass = v.w != 0.f;
    return v.xyz;
}

// Try to resolve hit lighting from screen space history directly (that spares the effort for further light ray tracing & shading)
// Dispatched per probe update ray, spawn candidate shading points
[numthreads(WAVE_SIZE, 1, 1)]
void ResolveHitLightingFromScreenHistoryAndSpecialEmitter (uint DispatchID : SV_DispatchThreadID) {
	int RayIndex = DispatchID;
    if(RayIndex >= RWVolumeProbeUpdateRayAllocator[0]) return ;
    float3 RayOrigin = RWVolumeProbeUpdateRayOriginBuffer[RayIndex];
    float3 RayDirection = RWVolumeProbeUpdateRayDirectionBuffer[RayIndex];
    uint  PackedRayState = RWVolumeProbeUpdateRayStateBuffer[RayIndex];
    bool bHit;
    float RayHitT = UnpackRayToTraceState(PackedRayState, bHit);
	CameraParameters C = GetActiveCamera();
	// Whether we should bypass the second level radiance cache. 
	bool bBypass = false;
	if(bHit && !UB.NoIndirectLighting) {
        uint2  PackedHitResult = RWVolumeProbeUpdateRayResultBuffer[RayIndex];
        CachedHitMaterial CM = UnpackCachedHitMaterial(PackedHitResult);
        // Specially, for GRF hits, simply decode color from hit albedo
        if(CM.HitType == CACHED_HIT_MATERIAL_HIT_TYPE_GAUSSIAN) {
            // Use a simple non-linear mapping to approximate radiance
            float3 Radiance = LinearColorToRadiance(CM.Albedo) * UB.GRF_EmitterIntensityScale;
            uint2 Packed = PackUpdateRayRadianceFlag(Radiance, true);
            bBypass = true;
            RWVolumeProbeUpdateRayRadianceBuffer[RayIndex] = Packed;
        } else {
            float3 HitWorldPosition = RayOrigin + RayDirection * RayHitT;
            CameraParameters PrevC = GetPreviousCamera();
            float4 PreviousHomogeneousW = mul(PrevC.WorldToNDC, float4(HitWorldPosition, 1));
            float3 PreviousHomogeneous = PreviousHomogeneousW.xyz / PreviousHomogeneousW.w;
            if(PreviousHomogeneousW.w > 0 && all(PreviousHomogeneous.xy >= -1) && all(PreviousHomogeneous.xy <= 1)
            && PreviousHomogeneous.z >= 0 && PreviousHomogeneous.z <= 1) {
                float2 HistoryScreenPosition = PrevC.FilmDimensions * NDC2ToUV(PreviousHomogeneous.xy);
                int2 HistoryScreenCoords = int2(HistoryScreenPosition + 0.5f);
                float3 HistoryNormal = normalize(PreviousNormalTexture.Load(int3(HistoryScreenCoords, 0)).xyz * 2.f - 1.f);
                if(CM.IsSurface()) {
                    // Surface hit, resolve from screen space history
                    float3 HitNormal      = CM.Normal;
                    bool   bNormalVisible = dot(HistoryNormal, HitNormal) > 0.5f;
                    float  HistoryReversedZDepth = PreviousDepthTexture.Load(int3(HistoryScreenCoords, 0)).x;
                    if(HistoryReversedZDepth > 0) {
                        float  HistoryDepth  = ReversedZDepthToLinearDepth(PrevC, HistoryReversedZDepth);
                        float  PreviousDepth = ZDepthToLinearDepth(PrevC, PreviousHomogeneous.z);
                        bool   bDepthVisible  = 
                                    abs(HistoryDepth - PreviousDepth) 
                                    / max(PreviousDepth, HistoryDepth) < 5e-2f;
                        if(bNormalVisible && bDepthVisible) {
                            // The irradiance is directly attained from reprojected history radiance
                            // The radiance has been multiplied by BRDF. So no need to do shading again.
                            float3 HistoryRadiance = PreviousShadedDiffuseRadianceWithoutEmission.Load(int3(HistoryScreenCoords, 0)).xyz;
                            bBypass = true;
                            uint2 Packed = PackUpdateRayRadianceFlag(HistoryRadiance, true);
                            RWVolumeProbeUpdateRayRadianceBuffer[RayIndex] = Packed;
                        }
                    }
                } else if(CM.IsVolume()) {
                    // Volume hit, resolve from volume history texture
                    float2 HistoryMinMax = PreviousVolumeMinMaxTexture.Load(int3(HistoryScreenCoords, 0)).xy;
                    // Relax min-max ranges a bit to avoid precision issues (fp16)
                    HistoryMinMax.x = max(0, HistoryMinMax.x * 0.999f);
                    HistoryMinMax.y = HistoryMinMax.y * 1.001f;
                    CameraParameters PrevC = GetPreviousCamera();
                    float3 PrevCamearDirection = NDC2ToCameraDirection(PrevC, PreviousHomogeneous.xy);
                    float  PrevCosineFactor = 1 / dot(PrevCamearDirection, PrevC.Direction);
                    float  PreviousLinearDepth = ZDepthToLinearDepth(PrevC, PreviousHomogeneous.z);
                    float  PreviousDistance = PreviousLinearDepth * PrevCosineFactor;
                    bool bDepthVisible = 
                        PreviousDistance >= HistoryMinMax.x &&
                        PreviousDistance <= HistoryMinMax.y;
                    if(UB.ScreenReuseNoDepthTesting || bDepthVisible) {
                        // Approximate the volume radiance from the volume history texture
                        // Assume that the volume radiance decreases exponentially with the depth
                        // and the overall integral equals to volume radiance
                        float4 HistoryVolumeRadiance = PreviousVolumeRadianceTexture.SampleLevel(PointEdgeSampler,
                                                    HistoryScreenPosition * C.InvFilmDimensions, 0);
                        float  HistoryVolumeDensity  = PreviousVolumeDensityTexture.Load(int3(HistoryScreenCoords, 0)).x;
                        float  MediaTraverseDistance = abs(PreviousDistance - HistoryMinMax.x);
                        float  MediaTransmittance = IntegrateExponentialScatteringMedium(HistoryVolumeDensity, MediaTraverseDistance);
                        float  HistoryTransmittance = PreviousTransmittanceTexture.SampleLevel(PointEdgeSampler,
                                                        HistoryScreenPosition * C.InvFilmDimensions, 0).x;
                        float  MediaLength = (HistoryMinMax.y - HistoryMinMax.x);
                        // Approximate the contribution of the current segment
                        float3 LiVirt = HistoryVolumeRadiance.rgb * saturate((1 - MediaTransmittance) / (1 - HistoryTransmittance));
                        // LiVirt = A / (tau - sigma) * (exp((tao-sigma) * m) - 1)
                        // where m = MediaLength, and LiVirt is reduced by approximating the medium contribution of the closest segment
                        float  NormalizationFactor = 1.f / (1.f + 0.02f - HistoryTransmittance);
                        float3 HistoryVolumeColor = PreviousVolumeColorTexture.SampleLevel(PointEdgeSampler,
                                                        HistoryScreenPosition * C.InvFilmDimensions, 0).xyz;
                        // Simple approximation
                        float3 EnergyDecay = max(saturate(1.f + 0.02f - HistoryVolumeColor), 0.02f);
                        float3 DepthEnergyDecayFactor_SimpleApproax = exp(-HistoryVolumeDensity * MediaTraverseDistance * (1 - HistoryVolumeColor));
                        float3 ApproximatedVolumeRadiance_SimpleApproax = LiVirt * NormalizationFactor * DepthEnergyDecayFactor_SimpleApproax;
                        // Modeled complex approximation (Gaussian energy decay model)
                        // L(z) = A * exp(-(z/sigma)^2), integrated with exponential transmittance exp(-Ext * z)
                        // Ratio of integrals cancels A and common factors, so we scale history radiance by I(d)/I(m)
                        float3 ApproximatedVolumeRadiance_ModeledApproax;
                        {
                            float  Ext = HistoryVolumeDensity;
                            float3 Albedo = HistoryVolumeColor;
                            float3 Sigma  = EstimateMultiScatteringRadianceDecayCurve_GaussianModel_Sigma(Ext, Albedo);
                            // Helper to compute erf arguments per-channel
                            float3 t0 = 0.5f * Ext * Sigma;                       // Ext * sigma / 2
                            float3 td = MediaTraverseDistance / Sigma + t0;       // d/sigma + Ext*sigma/2
                            float3 tm = MediaLength           / Sigma + t0;       // m/sigma + Ext*sigma/2
                            // Ratio = (erf(td) - erf(t0)) / (erf(tm) - erf(t0))
                            float3 num = erf_approx(td) - erf_approx(t0);
                            float3 den = erf_approx(tm) - erf_approx(t0);
                            float3 ratio = num / max(den, 1e-4f.xxx);
                            ratio = saturate(ratio);
                            ApproximatedVolumeRadiance_ModeledApproax = LiVirt * ratio * NormalizationFactor;
                        }
                        // Keep both approximations; choose simple as default, can swap if desired
                        // float3 FinalApproximatedVolumeRadiance = ApproximatedVolumeRadiance_SimpleApproax;
                        // Optionally prefer modeled approach when density is moderate and history is reliable
                        float3 FinalApproximatedVolumeRadiance = ApproximatedVolumeRadiance_ModeledApproax;
                        if(UB.NoScreenReuseEnergyDecay != 0) FinalApproximatedVolumeRadiance = LiVirt * NormalizationFactor;
                        bBypass = true;
                        uint2 Packed = PackUpdateRayRadianceFlag(FinalApproximatedVolumeRadiance, true);
                        RWVolumeProbeUpdateRayRadianceBuffer[RayIndex] = Packed;

                    }
                } else {
                    // Unknown material type, bypass (black)
                    bBypass = true;
                    RWVolumeProbeUpdateRayRadianceBuffer[RayIndex] = PackUpdateRayRadianceFlag(0.f.xxx, true);
                }
            }
        }
	}
    if(bHit && UB.NoIndirectLighting != 0) {
        // Bypass indirect lighting.
        bBypass = true;
        RWVolumeProbeUpdateRayRadianceBuffer[RayIndex] = PackUpdateRayRadianceFlag(0.f.xxx, true);
    }
	if(!bBypass) {
		if(bHit) {
            // Queue up all hits that failed in reprojection for world-space direct lighting
			uint HitCountNoBypass = WaveActiveCountBits(true);
			uint HitCountListOffset = 0;
			if(WaveIsFirstLane()) {
				InterlockedAdd(RWVolumeProbeUpdateRayHitShadingPointAllocator[0], HitCountNoBypass, HitCountListOffset);
			}
			HitCountListOffset = WaveReadLaneFirst(HitCountListOffset);
			
			uint ShadeHitIndex  = HitCountListOffset + WavePrefixCountBits(true);
			RWVolumeProbeUpdateRayHitShadingPointListBuffer[ShadeHitIndex] = RayIndex;

			// According to GI1.0, bypass the cache when the ray length from
			// primary vertex to secondary vertex is less than hash grid cell size.
			// (Avoid light leaking though cache filtering)
			float CellSize = HashGrids_GetCellSize(RayOrigin);
			bool bBypassCache = RayHitT < CellSize;
            RWVolumeProbeUpdateRayRadianceBuffer[RayIndex] = PackUpdateRayRadianceFlag(0, bBypassCache);
		} else {
			// A miss indicates that the ray has reached the sky
			// Sample the sky radiance and store it in the result buffer
			// Note: sky radiance is regarded an indirect lighting source
			// due to it's low frequency nature (sun excluded)
            float3 Radiance = EvaluateEnvironmentMap(-RayDirection);
            if(UB.NoEnvironmentLight != 0) Radiance = 0;
            RWVolumeProbeUpdateRayRadianceBuffer[RayIndex] = PackUpdateRayRadianceFlag(Radiance, true);
		}
	}
}

uint PackBucketSlotAndCellOffset(uint BucketSlotIndex, uint2 CellOffset) {
    if(!IsValid(BucketSlotIndex)) return INVALID_UINT;
    uint CellOffset1 = CellOffset.x + CellOffset.y * HASHGRIDS_TILE_CELL_WIDTH;
    return (BucketSlotIndex << (HASHGRIDS_TILE_CELL_WIDTH_L2 * 2)) 
    | (CellOffset1 & HASHGRIDS_TILE_CELL_INDEX_MASK);
}

void UnpackBucketSlotAndCellOffset(uint Packed, out uint BucketSlotIndex, out uint2 CellOffset) {
    if(IsInvalid(Packed)) {
        BucketSlotIndex = INVALID_UINT;
        CellOffset = INVALID_UINT;
        return ;
    }
    BucketSlotIndex = Packed >> (HASHGRIDS_TILE_CELL_WIDTH_L2 * 2);
    uint CellOffset1 = Packed & HASHGRIDS_TILE_CELL_INDEX_MASK;
    CellOffset = uint2(
        CellOffset1 % HASHGRIDS_TILE_CELL_WIDTH,
        CellOffset1 / HASHGRIDS_TILE_CELL_WIDTH
    );
}

// Sample light rays for DI calculation using light grid
// Dispatched per shade point

[numthreads(WAVE_SIZE, 1, 1)]
void SampleLightRaysForUpdateRayHits (uint DispatchID : SV_DispatchThreadID) {
    uint ShadePointIndex = DispatchID;
	if(ShadePointIndex >= RWVolumeProbeUpdateRayHitShadingPointAllocator[0]) return ;
	uint UpdateRayIndex = RWVolumeProbeUpdateRayHitShadingPointListBuffer[ShadePointIndex];
	float3 UpdateRayOrigin    = RWVolumeProbeUpdateRayOriginBuffer[UpdateRayIndex];
    bool bUpdateRayHit;
	float  UpdateRayDepth     = UnpackRayToTraceState(RWVolumeProbeUpdateRayStateBuffer[UpdateRayIndex], bUpdateRayHit);
	float3 UpdateRayDirection = RWVolumeProbeUpdateRayDirectionBuffer[UpdateRayIndex];
	float3 ShadePosition      = UpdateRayDirection * UpdateRayDepth + UpdateRayOrigin;
	float3 ShadeViewDirection = -UpdateRayDirection;
	// Till now rays to be traced have identical indices with the probe update rays
	// After this kernel, rays to be traced will be cleared and re-assigned shadow rays for DI calculation.
	uint2 PackedMaterial      = RWVolumeProbeUpdateRayResultBuffer[UpdateRayIndex];
	CachedHitMaterial ShadeMaterial = UnpackCachedHitMaterial(PackedMaterial);
	float3 ShadeNormal     = ShadeMaterial.Normal;
    CameraParameters C     = GetActiveCamera();

	// Offset the hit position to avoid self-intersection
    float ShadePositionOffsetLength = max(2e-5f, dot(abs(ShadePosition), 1.xxx) * 1e-5f);
	if(ShadeMaterial.IsSurface()) ShadePosition += ShadeNormal * ShadePositionOffsetLength;

    float3 ProbeNDC = TransformPoint(C.WorldToNDC, UpdateRayOrigin);
    float2 ProbeScreenUV = NDC2ToUV(ProbeNDC.xy);
    uint2  ProbeScreenCoords = uint2(ProbeScreenUV * C.FilmDimensions);
    Random R = MakeRandom(
        // Make random numbers consistent when freezing update ray seeds.
        14711712u + (ProbeScreenCoords.x * 6193 + ProbeScreenCoords.y) * MAX_NUM_UPDATE_RAYS_PER_PROBE
        + (asuint(UpdateRayDirection.x) + asuint(UpdateRayDirection.y) + asuint(UpdateRayDirection.z)),
        UB.ProbeUpdateRaySampleSeed
    );
    float  SumResampleWeights = 0;
    uint   NumValidSamples = 0;
    float  LightGridLightListCdf = 0;
    float3 ShadedRadiance = 0.f;
    LightSample ReservedSample = SampleOneLightSample_RIS(
        ShadePosition, ShadeNormal, ShadeViewDirection,
        ShadeMaterial.IsSurface(), false, true, true,
        R,
        ShadedRadiance,
        SumResampleWeights, NumValidSamples,
        LightGridLightListCdf
    );

	// Allocate an hash grid cache cell for the hit position
	// (Only points outside of the screen are cached in the hash grid cache)
	// Store indirections
    
    uint BucketSlotIndex = INVALID_UINT;
    uint2 CellOffset;
	uint AllocatedTileIndex = HashGrids_AllocateTile(
        ShadePosition, ShadeViewDirection, UpdateRayDepth,
        BucketSlotIndex, CellOffset
    );
	// Probe update ray results should be resolved from cell within the tile referred by the bucket slot
    // with corresponding cell offset.
	RWVolumeProbeUpdateRayHitResolveBucketAndCellOffsetBuffer[UpdateRayIndex]
        = PackBucketSlotAndCellOffset(BucketSlotIndex, CellOffset);

	// Spawn shadow ray
	float3 TransmittanceRayDirection         = 0;
	float TransmittanceRayOcclusionThreshold = 0;
	bool bValidRay = ReservedSample.IsValid() && dot(ShadedRadiance, 1.f.xxx) > 0;
    const float OcclusionEpsilon = 2e-3f; // 25.10.19: a too small value can cause false positives for shadow rays due to precision issues
	if(bValidRay) {

        if(ReservedSample.IsInfiniteLight()) {
            // Infinite light sample, trace to TMax
            TransmittanceRayDirection = ReservedSample.Position;
            TransmittanceRayOcclusionThreshold = C.FarPlane; // Far plane
        } else {
            TransmittanceRayDirection = normalize(ReservedSample.Position - ShadePosition);
            TransmittanceRayOcclusionThreshold = length(ReservedSample.Position - ShadePosition);
            // Avoid self-intersection
            float CoordinateEpsilon = max(TransmittanceRayOcclusionThreshold, dot(abs(ShadePosition), 1.xxx)) * OcclusionEpsilon;
            TransmittanceRayOcclusionThreshold = max(TransmittanceRayOcclusionThreshold - max(OcclusionEpsilon, CoordinateEpsilon), 0.f);
        }
		// Account for shading
        ShadedRadiance *= EvaluateCachedMaterialBRDF_ColorOnly(
            ShadeMaterial, ShadeViewDirection,
            TransmittanceRayDirection, VOLUME_PRIMITIVES_HENYEY_GREENSTEIN_PHASE_G
        );
	}

	// Allocate rays
	int TransmittanceRayIndex = INVALID_UINT;
	if(bValidRay) {
		int TransmittanceRayIndexBase = 0;
		int TransmittanceRayWarpCount = WaveActiveCountBits(1);
		int TransmittanceRayWarpRank  = WavePrefixCountBits(1);
		if(WaveIsFirstLane()) {
			InterlockedAdd(RWShadePointTransmittanceRayAllocator[0], TransmittanceRayWarpCount, TransmittanceRayIndexBase);
		}
		TransmittanceRayIndexBase = WaveReadLaneFirst(TransmittanceRayIndexBase);
		TransmittanceRayIndex = TransmittanceRayIndexBase + TransmittanceRayWarpRank;

	    // Write ray to memory for HWRT
        RWShadePointTransmittanceRayDirectionBuffer[TransmittanceRayIndex] = TransmittanceRayDirection;
        RWShadePointTransmittanceRayOriginBuffer[TransmittanceRayIndex]    = ShadePosition;
        RWShadePointTransmittanceRayStateBuffer[TransmittanceRayIndex]     = 0; // Initial state
        // Keep the occulusion threshold for direct illumination visibility testing
        RWShadePointTransmittanceRayTMaxBuffer[TransmittanceRayIndex]      = TransmittanceRayOcclusionThreshold;
        // Store the sample contribution for direct illumination (if it passed the visibility test)
        RWShadePointTransmittanceRayContributionBuffer[TransmittanceRayIndex] = PackFp16x4Safe(float4(ShadedRadiance, 1.f));
        // Keep sampled light record packed into a single uint.
        RWShadePointTransmittanceRaySampledLightIndexBuffer[TransmittanceRayIndex] = ReservedSample.LightRecord.Packed;
    }
    // Trace results are used to shade the hit of a probe update ray (shade point). Store indirections
    RWShadePointToTransmittanceRayIndexBuffer[ShadePointIndex] = TransmittanceRayIndex;
}

// Trace transmittance rays...

// Resolve direct lighting from transmittance ray (if valid) results, accumulate their contributions to hash grids
// Dispatched per shading point
[numthreads(WAVE_SIZE, 1, 1)]
void ResolveUpdateRayHitsDirectLightingFromTraceResult (uint DispatchID : SV_DispatchThreadID) {
	int ShadePointIndex = DispatchID;
	if(ShadePointIndex >= RWVolumeProbeUpdateRayHitShadingPointAllocator[0]) return ;
	CameraParameters C = GetActiveCamera();
	uint UpdateRayIndex = RWVolumeProbeUpdateRayHitShadingPointListBuffer[ShadePointIndex];
    uint TransmittanceRayIndex = RWShadePointToTransmittanceRayIndexBuffer[ShadePointIndex];

	float3 Radiance = 0;
    if(IsValid(TransmittanceRayIndex)) {
        float Transmittance = ShadePointTransmittanceRayTransmittanceBuffer[TransmittanceRayIndex];
		Radiance = UnpackFp16x4Safe(RWShadePointTransmittanceRayContributionBuffer[TransmittanceRayIndex]).xyz;
        bool bHit;
        float THit = UnpackRayToTraceState(RWShadePointTransmittanceRayStateBuffer[TransmittanceRayIndex], bHit);
        // If the ray hits a solit surface before reaching the light, it is occluded.
        // Otherwise just multiply the estimated ray transmittance.
		Radiance *= bHit ? 0 : Transmittance;
        float3 WorldPosition = RWShadePointTransmittanceRayOriginBuffer[TransmittanceRayIndex];
        float3 RayDirection  = RWShadePointTransmittanceRayDirectionBuffer[TransmittanceRayIndex];
        if(!bHit) {
            // Update light grid visibility for the sampled light
            LightSampleSrcLightRecord SampledLightRecord = (LightSampleSrcLightRecord)0;
            SampledLightRecord.Packed = RWShadePointTransmittanceRaySampledLightIndexBuffer[TransmittanceRayIndex];
            LightGrid_UpdateVisibilityForLightRecord(WorldPosition, RayDirection, SampledLightRecord);
        }
	}

	// Accumulate the radiance to the hash grid cell
    uint BucketSlotIndex = INVALID_UINT;
    uint2 CellOffset;
    UnpackBucketSlotAndCellOffset(
        RWVolumeProbeUpdateRayHitResolveBucketAndCellOffsetBuffer[UpdateRayIndex],
        BucketSlotIndex, CellOffset
    );
    if(IsValid(BucketSlotIndex)) {
        uint TileIndex = HashGrids_BucketTileIndexBuffer[BucketSlotIndex];
        if(IsValid(TileIndex)) {
            // Update tile timestamp and queue it up for update.
            HashGrids_TouchTile(TileIndex);
            uint CellIndex = HashGrids_GetCellIndex(TileIndex, CellOffset);
            uint CompactCellIndex = HashGrids_CellIndexToCompactCellIndex(CellIndex);
            // Clamp the outliers (due to inadequate light sampling)
            // Radiance = clamp(Radiance, 0, UB.II_SecondaryVertexRadianceClamping);
            // (temporary): use a fixed value (white) to update hash grids. Visualizing the 'hot' cells and tell them from hash grids that have not been updated.
            //Radiance = 1.f.xxx;
            HashGrids_AccumulateSamplesToCell(CompactCellIndex, Radiance, 1);
        }
    }
	// Bypass the hash grid cache for the probe update ray if required
    bool bBypass = false;
	float3 CurrentRadiance = UnpackUpdateRayRadianceFlag(RWVolumeProbeUpdateRayRadianceBuffer[UpdateRayIndex], bBypass);
	// Bypass hash grid cache, directly transfer radiance from DI results
    if(bBypass) {
        RWVolumeProbeUpdateRayRadianceBuffer[UpdateRayIndex] = PackUpdateRayRadianceFlag(CurrentRadiance + Radiance, true);
	}
}

// Hash grid cache update and filtering...

// Resolve probe update ray radiance results from hash grid cache
// Dispatched per shading point
[numthreads(WAVE_SIZE, 1, 1)]
void ResolveProbeUpdateRayRadianceFromCells (uint DispatchID : SV_DispatchThreadID)
{
    uint ShadingPointIndex = DispatchID;
	if(ShadingPointIndex >= RWVolumeProbeUpdateRayHitShadingPointAllocator[0]) return ;
	int UpdateRayIndex = RWVolumeProbeUpdateRayHitShadingPointListBuffer[ShadingPointIndex];
    uint BucketSlotIndex = INVALID_UINT;
    uint2 CellOffset;
    UnpackBucketSlotAndCellOffset(
        RWVolumeProbeUpdateRayHitResolveBucketAndCellOffsetBuffer[UpdateRayIndex],
        BucketSlotIndex, CellOffset
    );
    if(IsValid(BucketSlotIndex)) {
        uint TileIndex = HashGrids_BucketTileIndexBuffer[BucketSlotIndex];
        if(IsValid(TileIndex)) {
            uint CellIndex  = HashGrids_GetCellIndex(TileIndex, CellOffset);
            float4 Radiance = HashGrids_GetFilteredRadiance(CellIndex);
            bool bBypass;
            float3 OldRadiance = UnpackUpdateRayRadianceFlag(RWVolumeProbeUpdateRayRadianceBuffer[UpdateRayIndex], bBypass);
            if(!bBypass) {
                // Resolve radiance from hash grid cache if no bypass is specified
                float3 NewRadiance = Radiance.xyz + OldRadiance;
                uint2 Packed = PackUpdateRayRadianceFlag(NewRadiance, false);
                RWVolumeProbeUpdateRayRadianceBuffer[UpdateRayIndex] = Packed;
            }
        }
    }
}

// Update screen probes & cache
groupshared uint SharedProbeSampleWeightSums[TILE_SIZE * TILE_SIZE];
[numthreads(WAVE_SIZE, 1, 1)]
void UpdateVolumeProbesAndCache (uint GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint SpawnListIndex = GroupID;
    uint SpawnListCount = RWVolumeProbeSpawnAllocator[0];
    if(SpawnListIndex >= SpawnListCount) return;

    // Clear the shared memory for later ray radiance accumulation
    VolumeProbeHeader Header = UnpackVolumeProbeHeader(RWSpawnedVolumeProbeHeaderBuffer[SpawnListIndex]);
    CameraParameters C = GetActiveCamera();
    float3 ProbeNDC = TransformPoint(C.WorldToNDC, Header.WorldPosition);
    float2 ProbeScreenUV = NDC2ToUV(ProbeNDC.xy);
    uint2  ProbeScreenCoords = uint2(ProbeScreenUV * C.FilmDimensions);
    uint2 TileIndex  = ProbeScreenCoords / TILE_SIZE;
    uint  TileIndex1 = TileIndex.x + TileIndex.y * UB.TileDimensions.x;
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        SharedProbeBlendedRadiance[TexelIndex] = 0;
        SharedProbeSampleWeightSums[TexelIndex] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    float SumRayWeight = 0;
    float4 SumRayResult = 0;


    // Accumulate radiance from traced update rays
    uint ProbeUpdateRayBase = RWVolumeProbeUpdateRayOffsetsBuffer[SpawnListIndex];
    uint UpdateRayCount = RWVolumeProbeUpdateRayCountsBuffer[SpawnListIndex];
    
#ifdef DEBUG_OUTPUT_TRACED_RAY
    {
        uint2 DebugTileIndex = Debug.CursorScreenCoords / TILE_SIZE;
        if(all(TileIndex == DebugTileIndex)) {
            if(WaveIsFirstLane()) {
                RWDebugTracedRaysCount[0] = UpdateRayCount;
            }
            for(int BaseRayRank = 0; BaseRayRank < UpdateRayCount; BaseRayRank += WAVE_SIZE) {
                uint RayRank = BaseRayRank + LocalID;
                uint RayIndex = ProbeUpdateRayBase + RayRank;
                if(RayRank < UpdateRayCount) {
                    RWDebugTracedRayOrigins[RayRank] = ProbeWorldPos;
                    RWDebugTracedRayStates[RayRank] = RWVolumeProbeUpdateRayStateBuffer[RayIndex];
                    RWDebugTracedRayDirections[RayRank] = RWVolumeProbeUpdateRayDirectionBuffer[RayIndex];
                }
            }
        }
    }
#endif
    // Assume UpdateRayCount is a multiple of WAVE_SIZE, which is guaranteed by the previous shaders    
    for(uint BaseRayRank = 0; BaseRayRank < UpdateRayCount; BaseRayRank += WAVE_SIZE) {
        uint RayRank = BaseRayRank + LocalID;
        uint RayIndex = ProbeUpdateRayBase + RayRank;
        bool bBypass;
        // The hit distance is stored in RWVolumeProbeUpdateRayStateBuffer[RayIndex]
        bool bHit;
        float4 RayResult = 
            float4(
                UnpackUpdateRayRadianceFlag(RWVolumeProbeUpdateRayRadianceBuffer[RayIndex], bBypass),
                UnpackRayToTraceState(RWVolumeProbeUpdateRayStateBuffer[RayIndex], bHit)
            );
        float RayInvPdf = RWVolumeProbeUpdateRayInvPdfBuffer[RayIndex];
        bool bValid = RayInvPdf > 0 && RayResult.w > 0;
        if(bValid) {
            float3 RayDirection = RWVolumeProbeUpdateRayDirectionBuffer[RayIndex];
            float3 RayRadiance = RayResult.xyz;
            float2 RayOctahedronUV = UnitVectorToOctahedron01(RayDirection);
            uint2  RayTexelCoords = uint2(RayOctahedronUV * TILE_SIZE);
            uint   RayTexelIndex  = RayTexelCoords.x + RayTexelCoords.y * TILE_SIZE;
            float  RayTexelWeight = dSphericalAngle_dOctahedronArea01(RayDirection);
            float4 WeightedResult = float4(RayRadiance, RayResult.w) * RayTexelWeight;
            InterlockedAdd(SharedProbeBlendedRadiance[RayTexelIndex].x, QuantilizeRadiance(WeightedResult.x));
            InterlockedAdd(SharedProbeBlendedRadiance[RayTexelIndex].y, QuantilizeRadiance(WeightedResult.y));
            InterlockedAdd(SharedProbeBlendedRadiance[RayTexelIndex].z, QuantilizeRadiance(WeightedResult.z));
            InterlockedAdd(SharedProbeBlendedRadiance[RayTexelIndex].w, QuantilizeRadiance(WeightedResult.w));
            // Use RayTexelWeight here instead of simply using RayInvPdf for better quantilization quality
            InterlockedAdd(SharedProbeSampleWeightSums[RayTexelIndex], QuantilizeWeight(RayTexelWeight));
            SumRayWeight += RayInvPdf; // Here we need to use RayInvPdf to reweight bias introduced by importance sampling
            SumRayResult += float4(RayRadiance, RayResult.w) * RayInvPdf;
        }
    }

    SumRayWeight = WaveActiveSum(SumRayWeight);
    SumRayResult = WaveActiveSum(SumRayResult);
    float4 AverageRayResult = SumRayResult / max(SumRayWeight, 1e-5f);

    GroupMemoryBarrierWithGroupSync();

    float4 BackupRayResult = AverageRayResult;

    // Find a least recently used probe to overwrite. (Allocate the last N elements from the MRU queue)
    uint  OverwriteProbeIndex1 = RWVolumeProbeMRUQueueBuffer[UB.TileCount - SpawnListIndex - 1];
    uint2 OverwriteProbeIndex = uint2(
        OverwriteProbeIndex1 % UB.TileDimensions.x,
        OverwriteProbeIndex1 / UB.TileDimensions.x
    );

    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        uint2 TexelCoords = uint2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
        uint2 SrcAtlasTexelCoords = TileIndex * TILE_SIZE + TexelCoords;
        uint2 DstAtlasTexelCoords = OverwriteProbeIndex * TILE_SIZE + TexelCoords;
        uint4 NewRadianceQuantized = SharedProbeBlendedRadiance[TexelIndex];
        float SampleWeightSum = RecoverWeight(SharedProbeSampleWeightSums[TexelIndex]);
        float4 NewRadiance = RecoverRadiance(NewRadianceQuantized) / max(SampleWeightSum, 1e-3f);
        if(SampleWeightSum == 0) {
            NewRadiance = BackupRayResult;
        }
        // Temporal blending with the reconstructed radiance from previous frames.
        if (Header.bTemporalBlendable)
        {
            float4 ReconstructedRadiance = RWVolumeProbeReconstructedRadianceDepthTexture[SrcAtlasTexelCoords];
            float lumaA = RadianceToLuminance(NewRadiance.xyz);
            float lumaB = RadianceToLuminance(ReconstructedRadiance.xyz);

            // Unbiased blending
            float temporal_blend = 0.15f;
            
            NewRadiance = lerp(ReconstructedRadiance, NewRadiance, temporal_blend);
        }

        // Update foreground radiance weight atlas
        RWVolumeProbeRadianceDepthTexture[DstAtlasTexelCoords] = NewRadiance;
    }

    if(WaveIsFirstLane()) {
        // Finally, update the header for the newly spawned probe
        RWVolumeProbeHeaderTexture[OverwriteProbeIndex] = PackVolumeProbeHeader(Header);
        // Post-injection for indexing the new probe in this frame
        RWTileSpawnedVolumeProbeBuffer[TileIndex1] = OverwriteProbeIndex1;
    }
}

// Update MRU queue, putting recently used entries (last RWVolumeProbeSpawnAllocator elements) to the front
// TODO use touched flags
[numthreads(WAVE_SIZE, 1, 1)]
void UpdateVolumeProbeCacheMRUQueue (uint DispatchID : SV_DispatchThreadID) {
    uint QueueIndex = DispatchID;
    if(QueueIndex >= UB.TileCount) return;
    uint Value = 0;
    if(QueueIndex < RWVolumeProbeSpawnAllocator[0]) {
        uint Index = UB.TileCount - (RWVolumeProbeSpawnAllocator[0] - QueueIndex);
        Value = RWVolumeProbeMRUQueueBuffer[Index];
    } else {
        uint Index = QueueIndex - RWVolumeProbeSpawnAllocator[0];
        Value = RWVolumeProbeMRUQueueBuffer[Index];
    }
    RWVolumeProbeNextMRUQueueBuffer[QueueIndex] = Value;
}


void WriteVolumeProbeSHCoefficients (uint2 ProbeIndex, float3 SHCoefficients[9]) {
    float4 R1 = float4(SHCoefficients[1].x, SHCoefficients[2].x, SHCoefficients[3].x, SHCoefficients[4].x);
    float4 R2 = float4(SHCoefficients[5].x, SHCoefficients[6].x, SHCoefficients[7].x, SHCoefficients[8].x);
    float4 G1 = float4(SHCoefficients[1].y, SHCoefficients[2].y, SHCoefficients[3].y, SHCoefficients[4].y);
    float4 G2 = float4(SHCoefficients[5].y, SHCoefficients[6].y, SHCoefficients[7].y, SHCoefficients[8].y);
    float4 B1 = float4(SHCoefficients[1].z, SHCoefficients[2].z, SHCoefficients[3].z, SHCoefficients[4].z);
    float4 B2 = float4(SHCoefficients[5].z, SHCoefficients[6].z, SHCoefficients[7].z, SHCoefficients[8].z);
    float3 RGB0 = SHCoefficients[0];
    RWVolumeProbeIrradianceTexture[ProbeIndex].xyz = RGB0;
    RWVolumeProbeSHCoefficientsRTexture[ProbeIndex] = R1;
    RWVolumeProbeSHCoefficientsRTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)] = R2;
    RWVolumeProbeSHCoefficientsBTexture[ProbeIndex] = B1;
    RWVolumeProbeSHCoefficientsBTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)] = B2;
    RWVolumeProbeSHCoefficientsGTexture[ProbeIndex] = G1;
    RWVolumeProbeSHCoefficientsGTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)] = G2;
}

void GetVolumeProbeSHCoefficients (uint2 ProbeIndex, out float3 ProbeSH[9]) {
    float4 R1 = RWVolumeProbeSHCoefficientsRTexture[ProbeIndex];
    float4 R2 = RWVolumeProbeSHCoefficientsRTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)];
    float4 G1 = RWVolumeProbeSHCoefficientsGTexture[ProbeIndex];
    float4 G2 = RWVolumeProbeSHCoefficientsGTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)];
    float4 B1 = RWVolumeProbeSHCoefficientsBTexture[ProbeIndex];
    float4 B2 = RWVolumeProbeSHCoefficientsBTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)];
    float3 RGB0 = RWVolumeProbeIrradianceTexture[ProbeIndex].xyz;
    ProbeSH[0] = RGB0;
    ProbeSH[1] = float3(R1.x, G1.x, B1.x);
    ProbeSH[2] = float3(R1.y, G1.y, B1.y);
    ProbeSH[3] = float3(R1.z, G1.z, B1.z);
    ProbeSH[4] = float3(R1.w, G1.w, B1.w);
    ProbeSH[5] = float3(R2.x, G2.x, B2.x);
    ProbeSH[6] = float3(R2.y, G2.y, B2.y);
    ProbeSH[7] = float3(R2.z, G2.z, B2.z);
    ProbeSH[8] = float3(R2.w, G2.w, B2.w);
}

[numthreads(WAVE_SIZE, 1, 1)]
void ComputeVolumeProbeSHCoefficients (uint2 GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint2 ProbeIndex = GroupID;
    VolumeProbeHeader Header = UnpackVolumeProbeHeader(RWVolumeProbeHeaderTexture[ProbeIndex]);
    if(!Header.bActive) return;
    CameraParameters C = GetActiveCamera();
    // float2 ProbeUV = (Header.PixelCoords + 0.5f) * C.InvFilmDimensions;
    // float3 ProbeNormal = normalize(G_Normal.SampleLevel(PointEdgeSampler, ProbeUV, 0).rgb * 2 - 1);
    // float3 ProbeTangent, ProbeBitangent;
    // GetOrthoVectors(ProbeNormal, ProbeTangent, ProbeBitangent);

    uint2 ProbeAtlasBaseCoords = ProbeIndex * TILE_SIZE;

    // Clear SH coefficients
    float3 SHCoefficients[9];
    [unroll] for(uint i = 0; i < 9; i++) {
        SHCoefficients[i] = 0;
    }

    float SumAreaCorrectionFactor = 0.f;
    // Accumulate SH coefficients
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        uint2 TexelCoords = uint2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
        uint2 AtlasTexelCoords = ProbeAtlasBaseCoords + TexelCoords;
        float4 RadianceDepth = RWVolumeProbeRadianceDepthTexture[AtlasTexelCoords];
        float3 Radiance = RadianceDepth.xyz;
        float2 TexelUV = (float2(TexelCoords) + 0.5f) / TILE_SIZE;
        float3 TexelDirection = Octahedron01ToUnitVector(TexelUV);
        // Approximated with center sample differentials
        float AreaCorrectionFactor = dSphericalAngle_dOctahedronArea01(TexelDirection);
        // Accumulate SH coefficients
        float  Coefficients[9];
        SH_GetCoefficients(TexelDirection, Coefficients);
        for(int i = 0; i < 9; i++) {
            SHCoefficients[i] += Coefficients[i] * Radiance * AreaCorrectionFactor;
        }
        SumAreaCorrectionFactor += AreaCorrectionFactor;
    }
    SumAreaCorrectionFactor = WaveActiveSum(SumAreaCorrectionFactor);
    // Write SH coefficients
    for(uint i = 0; i<9; i++) {
        // Multiply by FOUR_PI to monte-carlo integrate to retrieve the coefficients
        // Note: the area correction factor has been applied. So no extra area correction is needed here.
        // (mapping square to sphere)
        SHCoefficients[i] = WaveActiveSum(SHCoefficients[i]) * (1.f / TILE_TEXEL_COUNT);
    }
    if(WaveIsFirstLane()) {
        WriteVolumeProbeSHCoefficients(ProbeIndex, SHCoefficients);
    }
}

// Modified from GI1.0
// Evaluates the irradiance from the probe's SH representation using a bent cone.
float3 ProbeIntegrateHenyeyGreenstein(float3 ViewDirection, float g, uint2 ProbeIndex)
{
    float PhaseSH[9];
    SH_GetCoefficients_HenyeyGreenstein(ViewDirection, g, PhaseSH);

    float3 Irradiance = float3(0.0f, 0.0f, 0.0f);
    float3 ProbeSH[9];
    GetVolumeProbeSHCoefficients(ProbeIndex, ProbeSH);
    for (uint i = 0; i < 9; ++i)
    {
        Irradiance += PhaseSH[i] * ProbeSH[i];
    }

    return max(Irradiance, 0.0f);
}


// Shade volume with indirect lighting from probes
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void ComputeVolumeIndirectLighting (uint2 GroupID : SV_GroupID, uint2 LocalID : SV_GroupThreadID) {
    uint2 PixelCoords = GroupID * TILE_SIZE + LocalID;
    CameraParameters C = GetActiveCamera();
    if (any(PixelCoords >= C.FilmDimensions)) return;
    float  SampleDepth = G_VolumeSampleDepth.Load(int3(PixelCoords, 0));
    float3 SampleColor = G_VolumeSampleColor.Load(int3(PixelCoords, 0)).xyz;
    if (SampleDepth == 0)
    {
        RWVolumeIndirectLightingTexture[PixelCoords] = 0;
        return; 
    }
    float2 ScreenPos = PixelCoords + 0.5f;
    float2 UV = ScreenPos * C.InvFilmDimensions;
    float3 ViewDirection = NDC2ToCameraDirection(C, UVToNDC2(UV));
    float3 WorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(UV), SampleDepth);
    float  PixelSearchSize = UB.ProbeReprojectionSearchSize;
    float  SurfaceDepthSearchSize = PixelSearchSize * max(C.FilmPixelWorldSize.x, C.FilmPixelWorldSize.y) * SampleDepth;
    // Along the camera's z-axis, the search size solely depends on volume density
    float  VolumeDensity = VolumeDensityTexture.SampleLevel(PointEdgeSampler, UV, 0).r;
    float  ToLinear = dot(C.Direction, normalize(WorldPosition - C.Position));
    float  SurfaceLinearDepth = VolumeMinMaxTexture.SampleLevel(PointEdgeSampler, UV, 0).r * ToLinear;
    float  VolumeDepthSearchSize = GetProbeDepthSearchSizeForDensity(VolumeDensity);
    uint2  TileIndex = PixelCoords / TILE_SIZE;
    float  SumProbeWeights = 0;
    float3 SumIrradiance = 0;

    float g = 0.0f; // Lambertian

    int2 Corner = select((PixelCoords % TILE_SIZE) < (TILE_SIZE / 2), -1.xx, 0.xx);
    for(int dX = 0; dX < 2; dX ++) {
        for(int dY = 0; dY < 2; dY ++) {
            int2 SearchTileIndex = int2(TileIndex) + int2(Corner.x + dX, Corner.y + dY);
            if(any(SearchTileIndex < 0) || any(SearchTileIndex >= int2(UB.TileDimensions))) {
                continue;
            }
            uint SearchTileIndex1 = uint(
                SearchTileIndex.x + SearchTileIndex.y * UB.TileDimensions.x
            );
            // Check for newly spawned probe first
            if(RWTileSpawnedVolumeProbeBuffer[SearchTileIndex1] != INVALID_UINT) {
                uint SpawnedProbeIndex1 = RWTileSpawnedVolumeProbeBuffer[SearchTileIndex1];
                uint2 SpawnedProbeIndex = uint2(
                    SpawnedProbeIndex1 % UB.TileDimensions.x,
                    SpawnedProbeIndex1 / UB.TileDimensions.x
                );
                VolumeProbeHeader SpawnedProbeHeader = 
                    UnpackVolumeProbeHeader(RWVolumeProbeHeaderTexture[SpawnedProbeIndex]);
                if(SpawnedProbeHeader.bActive) {
                    float3 ProbeWorldPosition = SpawnedProbeHeader.WorldPosition;
                    // Check for probe occlusion
                    float3 ProbeNDC = TransformPoint(C.WorldToNDC, ProbeWorldPosition);
                    float2 ProbeScreenUV = NDC2ToUV(ProbeNDC.xy);
                    float2 ProbeScreenPos = ProbeScreenUV * C.FilmDimensions;
                    float  ProbeToLinear = dot(C.Direction, normalize(ProbeWorldPosition - C.Position));
                    float  ProbeSurfaceLinearDepth = VolumeMinMaxTexture.SampleLevel(PointEdgeSampler, ProbeScreenUV, 0).r * ProbeToLinear;
                    float  ProbeLinearDepth = dot(C.Direction, ProbeWorldPosition - C.Position);
                    if(TestProbeOcclusion(
                        ScreenPos, SampleDepth, SurfaceLinearDepth, 
                        ProbeScreenPos, ProbeLinearDepth, ProbeSurfaceLinearDepth,
                        PixelSearchSize, VolumeDepthSearchSize, SurfaceDepthSearchSize
                    )) {
                        float PixelDistance = length(ProbeScreenPos - ScreenPos);
                        float ProbeWeight = saturate(1 - PixelDistance / PixelSearchSize);
                        SumProbeWeights += ProbeWeight;
                        float3 Irradiance = ProbeIntegrateHenyeyGreenstein(ViewDirection, g, SpawnedProbeIndex);
                        SumIrradiance += ProbeWeight * Irradiance * SampleColor;
                    }
                }
            }
        }
    }

    bool bShadingIncomplete = SumProbeWeights < 1.f;

    if(bShadingIncomplete) {
        for(int dX = 0; dX < 2; dX ++) {
            for(int dY = 0; dY < 2; dY ++) {
                int2 SearchTileIndex = int2(TileIndex) + int2(Corner.x + dX, Corner.y + dY);
                if(any(SearchTileIndex < 0) || any(SearchTileIndex >= int2(UB.TileDimensions))) {
                    continue;
                }
                uint SearchTileIndex1 = uint(
                    SearchTileIndex.x + SearchTileIndex.y * UB.TileDimensions.x
                );
                // Check for previous probes (injected to the tile-probe index)
                uint NumProbesInTile     = RWTileVolumeProbeIndexListLengthsBuffer[SearchTileIndex1];
                uint TileProbeListOffset = RWTileVolumeProbeIndexListOffsetsBuffer[SearchTileIndex1];
                for(uint Rank = 0; Rank < NumProbesInTile; Rank ++) {
                    uint  CurrentProbeIndex1 = RWTileVolumeProbeIndexListBuffer[TileProbeListOffset + Rank];
                    uint2 CurrentProbeIndex  = uint2(
                        CurrentProbeIndex1 % UB.TileDimensions.x,
                        CurrentProbeIndex1 / UB.TileDimensions.x
                    );
                    VolumeProbeHeader CurrentProbeHeader = 
                        UnpackVolumeProbeHeader(RWVolumeProbeHeaderTexture[CurrentProbeIndex]);
                    if(CurrentProbeHeader.bActive) {
                        float3 ProbeWorldPosition = CurrentProbeHeader.WorldPosition;
                        // Check for probe occlusion
                        float3 ProbeNDC = TransformPoint(C.WorldToNDC, ProbeWorldPosition);
                        float2 ProbeScreenUV = NDC2ToUV(ProbeNDC.xy);
                        float2 ProbeScreenPos = ProbeScreenUV * C.FilmDimensions;
                        float  ProbeToLinear = dot(C.Direction, normalize(ProbeWorldPosition - C.Position));
                        float  ProbeSurfaceLinearDepth = VolumeMinMaxTexture.SampleLevel(PointEdgeSampler, ProbeScreenUV, 0).r * ProbeToLinear;
                        float  ProbeLinearDepth = dot(C.Direction, ProbeWorldPosition - C.Position);
                        if(TestProbeOcclusion(
                            ScreenPos, SampleDepth, SurfaceLinearDepth, 
                            ProbeScreenPos, ProbeLinearDepth, ProbeSurfaceLinearDepth,
                            PixelSearchSize, VolumeDepthSearchSize, SurfaceDepthSearchSize
                        )) {
                            float PixelDistance = length(ProbeScreenPos - ScreenPos);
                            float ProbeWeight = saturate(1 - PixelDistance / PixelSearchSize);
                            SumProbeWeights += ProbeWeight;
                            float3 Irradiance = ProbeIntegrateHenyeyGreenstein(ViewDirection, g, CurrentProbeIndex);
                            SumIrradiance += ProbeWeight * Irradiance * SampleColor;
                        }
                    }
                }
            }
        }
    }

    if(SumProbeWeights > 0.01f) {
        float3 Irradiance = (SumIrradiance / SumProbeWeights);
        RWVolumeIndirectLightingTexture[PixelCoords] = float4(Irradiance, 1);
    } else {
        RWVolumeIndirectLightingTexture[PixelCoords] = 0;
    }
}

RWStructuredBuffer<uint> RWDebugVolumeProbePositionsCount;
RWStructuredBuffer<float3> RWDebugVolumeProbePositionsBuffer;

[numthreads(WAVE_SIZE, 1, 1)]
void DebugOutputVolumeProbePositions (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID == 0) {
        RWDebugVolumeProbePositionsCount[0] = RWActiveVolumeProbeCount[0];
    }
    uint ProbeListIndex = DispatchID;
    if(ProbeListIndex >= RWActiveVolumeProbeCount[0]) return;
    uint ProbeIndex1 = RWActiveVolumeProbeListBuffer[ProbeListIndex];
    uint2 ProbeIndex = uint2(
        ProbeIndex1 % UB.TileDimensions.x,
        ProbeIndex1 / UB.TileDimensions.x
    );
    VolumeProbeHeader Header = UnpackVolumeProbeHeader(RWVolumeProbeHeaderTexture[ProbeIndex]);
    RWDebugVolumeProbePositionsBuffer[ProbeListIndex] = Header.WorldPosition;
}
