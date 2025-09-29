#include "headers/Conventions.hlsl"
#include "headers/Camera.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Random.hlsl"
#include "headers/OctahedronMapping.hlsl"
#include "headers/Radiometry.hlsl"
#include "headers/Sampling.hlsl"
#include "resources/CommonSamplerResources.hlsl"

// Screen probes
[[vk::image_format(rgba16f)]]
Texture2D<float4> PreviousScreenProbeRadianceDepthTexture;
[[vk::image_format(rgba16f)]]
RWTexture2D<float4> RWScreenProbeRadianceDepthTexture;

// Probe cache indexing and updating datastructures
StructuredBuffer<uint> PreviousTileScreenProbeCacheListIndexBuffer; // stores a index to the probe cache entry. used for tiles to query cached probes from last frame
StructuredBuffer<uint> PreviousTileScreenProbeCacheListLengthsBuffer;
StructuredBuffer<uint> PreviousTileScreenProbeCacheListOffsetsBuffer; // used to index into PreviousTileScreenProbeCacheListIndexBuffer from tiles
StructuredBuffer<uint> PreviousScreenProbeCacheEntryCount; // Number of all cached probes
StructuredBuffer<float4> PreviousScreenProbeCacheDataBuffer; // Cache entries. World position + normal

RWStructuredBuffer<uint> RWTileScreenProbeCacheIndexListBuffer;
RWStructuredBuffer<uint> RWTileScreenProbeCacheIndexListLengthsBuffer;
RWStructuredBuffer<uint> RWTileScreenProbeCacheIndexListOffsetsBuffer;
RWStructuredBuffer<uint> RWScreenProbeCacheEntryCount;

RWStructuredBuffer<uint> RWScreenProbeTileIndexListAllocator; // Used to allocate RWTileScreenProbeCacheIndexListBuffer entries to tiles

// Temporary buffers serving the reprojection of probe cache and rebuilding of the tile index list of cached probes.
RWStructuredBuffer<uint4> RWScreenProbeCacheIndexReprojectionEntryBuffer;
RWStructuredBuffer<uint> RWScreenProbeCacheIndexReprojectionCountBuffer;

// Buffers holding the cache entries to update & evict upon probe spawning.
// Indexed with SpawnListIndex
RWStructuredBuffer<uint2> RWScreenProbeSpawnCacheMatchesBuffer; // cache entry of probe to evict, cache entry of probe to update via spawned probe

// Probe cache (Backup for screen probes. Evicted probes will be stored here)
RWStructuredBuffer<float4> RWScreenProbeCacheDataBuffer; // Cached data (world position + normal)
[[vk::image_format(rgba16f)]]
RWTexture2D<float4> RWScreenProbeCacheRadianceDepthBuffer; // Cached radiance & depth
RWStructuredBuffer<uint> RWScreenProbeCacheTimestampBuffer; // Cache ages
RWStructuredBuffer<float> RWScreenProbeCacheFreeListBuffer; // Free list of unused cache entries
RWStructuredBuffer<uint> RWScreenProbeCacheFreeListAllocator; // Number of free cache entries

// A mipmapped texture (of resolution TileDimensions), including the closest probe header within tiles.
Texture2D<uint> PreviousTileScreenProbeHeaderTexture;
// Mip 0
RWTexture2D<uint> RWTileScreenProbeHeaderTexture;

// The list of reprojection occlusion tiles
RWStructuredBuffer<uint> RWReprojectionFailTileCountBuffer;
RWStructuredBuffer<uint> RWReprojectionFailTileListBuffer; // uint16x2 packed tile index

// The list of new probes to spawn in this frame
// + FailTileCountBuffer = real number of probes to spawn
RWStructuredBuffer<uint> RWScreenProbeSpawnCountBuffer;
// Packed probe headers to be spawned
RWStructuredBuffer<uint> RWScreenProbeSpawnListBuffer;

// Spawned rays
RWStructuredBuffer<uint> RWScreenProbeUpdateRayOffsetsBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayCountsBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayDirectionBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayOriginScreenCoordsBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayAllocator; // Number of all rays to be traced


RWStructuredBuffer<uint2> RWScreenProbeUpdateRayResultBuffer;
RWStructuredBuffer<float> RWScreenProbeUpdateRayInvPdfBuffer;

Texture2D<float> G_Depth;
Texture2D<float3> G_Normal;

Texture2D<float> PreviousDepthTexture;
Texture2D<float3> PreviousNormalTexture;

struct DiffuseIndirectLightingUB {
    uint2 TileSize;
    uint2 TileDimensions;

    float2 InvTileDimensions;
    uint  TileCount;
    float InvTileCount;

    float ProbeReprojectionSearchSize;
    uint  MaxProbesToSpawnPerFrame;
    float2 InvProbeAtlasDimensions;
    
    uint FrameIndex;
    uint ProbeUpdateRaysNoImportanceSampling;
    uint2 Padding;
};

ConstantBuffer<DiffuseIndirectLightingUB> UB;

uint2 UnpackProbeIndex (uint ProbeIndex) {
    return uint2(ProbeIndex / UB.TileDimensions.x, ProbeIndex % UB.TileDimensions.x);
}

uint GetProbeCacheEntryIndex (uint ProbeIndex1) {
    return ProbeIndex1 - UB.TileCount;
}

struct ScreenProbeHeader {
    bool bValid;
    uint2 PixelCoords;
};

ScreenProbeHeader UnpackProbeHeader (uint ProbeHeader) {
    ScreenProbeHeader Header;
    Header.bValid = !(ProbeHeader & 0x80000000u);
    Header.PixelCoords = uint2(ProbeHeader & 0xFFFFu, (ProbeHeader >> 16) & 0x7FFFu);
    return Header;
}

uint PackProbeHeader (ScreenProbeHeader Header) {
    uint ProbeHeader = 0;
    ProbeHeader |= Header.bValid ? 0 : 0x80000000u;
    ProbeHeader |= Header.PixelCoords.x | (Header.PixelCoords.y << 16);
    return ProbeHeader;
}

// Quantilization
// May overflow if the radiance is too large (e.g. 1000)
int QuantilizeRadiance (float V, float Noise = 0) {
    return floor(sign(V) * min(abs(V), 1000) * 16384 + Noise);
}
int4 QuantilizeRadiance (float4 V, float Noise = 0) {
    return int4(
        QuantilizeRadiance(V.x, Noise), QuantilizeRadiance(V.y, Noise),
        QuantilizeRadiance(V.z, Noise), QuantilizeRadiance(V.w, Noise));
}

float RecoverRadiance (int V) {
    return float(V) / 16384;
}
float3 RecoverRadiance (int3 V) {
    return float3(V) / 16384;
}
float4 RecoverRadiance (int4 V) {
    return float4(V) / 16384;
}
int QuantilizeWeight (float V, float Noise = 0) {
    // 2^17 = 131,072 (fp32: 2^23 precision)
    // Note: InvPdf < 100, no worries about overflowing
    return floor(V * 131072.f + Noise);
}
float RecoverWeight (int V) {
    return float(V) / 131072.f;
}

#ifndef TILE_SIZE
#define TILE_SIZE 8
#endif

#ifdef TILE_SIZE
    #if TILE_SIZE != 8
    #error "TILE_SIZE must be 8"
    #endif
#endif

#define TILE_TEXEL_COUNT (TILE_SIZE * TILE_SIZE)
#define TILE_TEXEL_COUNT_L2 6

#ifndef WAVE_SIZE
#define WAVE_SIZE 32
#endif

#if TILE_TEXEL_COUNT % WAVE_SIZE != 0
#error "TILE_TEXEL_COUNT must be a multiple of WAVE_SIZE"
#endif

[numthread(1, 1, 1)]
void ClearCounters () {
    RWScreenProbeSpawnCountBuffer[0] = 0;
    RWReprojectionFailTileCountBuffer[0] = 0;
}

groupshared uint SharedScreenProbeSourceHeader;
groupshared uint SharedReprojectedRadiance[TILE_SIZE * TILE_SIZE * 4];
groupshared uint SharedReprojectedSampleCounts[TILE_SIZE * TILE_SIZE];

// Spawn probes from on-screen probes in the previous frame
[numthreads(WAVE_SIZE, 1, 1)]
void ReprojectScreenProbes (uint2 GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint2 TileIndex = GroupID.xy;
    CameraParameters C = GetActiveCamera();

    // Clear the reprojected radiance
    for(uint WaveBaseIndex = 0; WaveBaseIndex < TILE_TEXEL_COUNT; WaveBaseIndex += WAVE_SIZE) {
        uint TexelIndex = WaveBaseIndex + LocalID;
        SharedReprojectedRadiance[TexelIndex * 4 + 0] = 0;
        SharedReprojectedRadiance[TexelIndex * 4 + 1] = 0;
        SharedReprojectedRadiance[TexelIndex * 4 + 2] = 0;
        SharedReprojectedRadiance[TexelIndex * 4 + 3] = 0;
        SharedReprojectedSampleCounts[TexelIndex] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    uint MinScreenProbeScore = 0xFFFFFFFFu;
    
    for(uint WaveBaseIndex = 0; WaveBaseIndex < TILE_TEXEL_COUNT; WaveBaseIndex += WAVE_SIZE) {
        uint TexelIndex = WaveBaseIndex + LocalID;
        uint2 PixelCoords = TileIndex * TILE_SIZE + uint2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
        float2 UV = ScreenCoordsToUV(C, PixelCoords);
        float ReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, UV, 0);
        float3 Normal = normalize(G_Normal.SampleLevel(PointEdgeSampler, UV, 0).rgb * 2 - 1);
        bool bValidPixel = ReversedZDepth > 0;
        float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
        float ZDepth = 1 - ReversedZDepth;
        float3 NDC = float3(UVToNDC2(UV), ZDepth);
        float3 WorldPosition = RecoverWorldPositionNDC2(C, NDC.xy, LinearDepth);

        float SearchSize = LinearDepth * UB.ProbeReprojectionSearchSize;
        
        uint PreviousProbeHeaderPacked = 0xFFFFFFFFu;
        if(bValidPixel) {
            float3 PreviousNDC = ReprojectToPreviousNDCFromNDC(C, NDC);
            float2 PreviousUV = NDC2ToUV(PreviousNDC.xy);
            if(all(PreviousUV >= 0 && PreviousUV < 1)) {
                uint2 PreviousTile = floor(PreviousUV * UB.TileDimensions);
                PreviousProbeHeaderPacked = PreviousTileScreenProbeHeaderTexture.Load(int3(PreviousTile, 0));
                ScreenProbeHeader PreviousProbe = UnpackProbeHeader(PreviousProbeHeaderPacked);
                // Search the previous tiles for a valid probe with the best score.
                if(PreviousProbe.bValid) {
                    float2 PreviousPixelCoords = PreviousProbe.PixelCoords;
                    float2 PreviousUV = ScreenCoordsToUV(C, PreviousPixelCoords);
                    float PreviousReversedZDepth = PreviousDepthTexture.SampleLevel(PointEdgeSampler, PreviousUV, 0);
                    float3 PreviousProbeWorldPosition = RecoverWorldPositionNDC2(C, PreviousUV, ReversedZDepthToLinearDepth(C, PreviousReversedZDepth));
                    float3 PreviousProbeWorldNormal = normalize(PreviousNormalTexture.SampleLevel(PointEdgeSampler, PreviousUV, 0).rgb * 2 - 1);

                    if(abs(dot(PreviousProbeWorldPosition - WorldPosition, Normal)) <= SearchSize && dot(PreviousProbeWorldNormal, Normal) > 0.95f) {
                        uint ProbeScore = f32tof16(distance(PreviousProbeWorldPosition, WorldPosition) / SearchSize) << 16 | TexelIndex;
                        MinScreenProbeScore = WaveActiveMin(min(MinScreenProbeScore, ProbeScore));
                    }
                    if(TexelIndex == (MinScreenProbeScore & 0xFFFF)) {
                        SharedScreenProbeSourceHeader = PreviousProbeHeaderPacked;
                    }
                }
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    uint SelectedSrcProbeThread = MinScreenProbeScore & 0xFFFFu;
    uint2 SelectedPixelIndex = uint2(SelectedSrcProbeThread % TILE_SIZE, SelectedSrcProbeThread / TILE_SIZE) + TileIndex * TILE_SIZE;
    
    if (SelectedSrcProbeThread != 0xFFFFu)
    {
        ScreenProbeHeader PrevProbeHeader = UnpackProbeHeader(SharedScreenProbeSourceHeader);
        float SelectedPixelLinearDepth = ReversedZDepthToLinearDepth(C, G_Depth.Load(int3(SelectedPixelIndex, 0)).x);
        float2 SelectdPixelUV = ScreenCoordsToUV(C, SelectedPixelIndex);
        float3 SelectedPixelWorldPosition = RecoverWorldPositionPixelCoords(C, SelectedPixelIndex, SelectedPixelLinearDepth);
        float3 SelectedPixelWorldNormal = normalize(G_Normal.SampleLevel(PointEdgeSampler, SelectdPixelUV, 0).rgb * 2 - 1);

        CameraParameters PrevC = GetPreviousCamera();
        float2 PrevProbeUV = ScreenCoordsToUV(PrevC, PrevProbeHeader.PixelCoords);
        float3 PrevProbeNormal = normalize(PreviousNormalTexture.SampleLevel(PointEdgeSampler, PrevProbeUV, 0).rgb * 2 - 1);
        float PrevProbeLinearDepth = ReversedZDepthToLinearDepth(PrevC, PreviousDepthTexture.Load(int3(PrevProbeHeader.PixelCoords, 0)).x);
        float3 PrevProbeWorldPosition = RecoverWorldPositionPixelCoords(PrevC, PrevProbeHeader.PixelCoords, PrevProbeLinearDepth);

        for(int BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
            uint TexelIndex = BaseTexelIndex + LocalID;
            uint2 TexelOffset = uint2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
            // Start reprojection
            float2 HistoryRadianceUV    = (PrevProbeHeader.PixelCoords / TILE_SIZE + TexelOffset) * UB.InvProbeAtlasDimensions;
            float4 HistoryRadianceDepth = PreviousScreenProbeRadianceDepthTexture.SampleLevel(PointEdgeSampler, HistoryRadianceUV, 0);
            float3 HistoryRadianceLocalDirection = HemiOctahedron01ToUnitVectorA((TexelOffset + 0.5f) / TILE_SIZE);

            float3 Tangent, Bitangent;
            GetOrthoVectors(PrevProbeNormal, Tangent, Bitangent);
            float3 HistoryRadianceWorldDirection = 
                Tangent * HistoryRadianceLocalDirection.x 
                + Bitangent * HistoryRadianceLocalDirection.y
                + PrevProbeNormal * HistoryRadianceLocalDirection.z;

            float3 HistoryHitPoint = PrevProbeWorldPosition + HistoryRadianceWorldDirection * HistoryRadianceDepth.w;
            float3 ReprojectedWorldDirection = HistoryHitPoint - SelectedPixelWorldPosition;
            float  ReprojectedDepth = length(ReprojectedWorldDirection);

            ReprojectedWorldDirection /= ReprojectedDepth; // normalize

            if (dot(SelectedPixelWorldNormal, ReprojectedWorldDirection) > 0.0f)
            {
                GetOrthoVectors(SelectedPixelWorldNormal, Tangent, Bitangent);
                float3 ReprojectedLocalDirection = 
                    float3(dot(ReprojectedWorldDirection, Tangent), 
                        dot(ReprojectedWorldDirection, Bitangent), 
                        dot(ReprojectedWorldDirection, SelectedPixelWorldNormal));
                float2 ReprojectedProbeUV = UnitVectorToHemiOctahedron01A(ReprojectedLocalDirection);
                uint2 ReprojectedProbePixelCoords = uint2(ReprojectedProbeUV * TILE_SIZE);
                uint ReprojectedProbeTexelIndex = ReprojectedProbePixelCoords.x + ReprojectedProbePixelCoords.y * TILE_SIZE;
                
                uint4  QuantilizedRadiance   = QuantilizeRadiance(float4(HistoryRadianceDepth.xyz, ReprojectedDepth));

                InterlockedAdd(SharedReprojectedRadiance[(ReprojectedProbeTexelIndex * 4) + 0], QuantilizedRadiance.x);
                InterlockedAdd(SharedReprojectedRadiance[(ReprojectedProbeTexelIndex * 4) + 1], QuantilizedRadiance.y);
                InterlockedAdd(SharedReprojectedRadiance[(ReprojectedProbeTexelIndex * 4) + 2], QuantilizedRadiance.z);
                InterlockedAdd(SharedReprojectedRadiance[(ReprojectedProbeTexelIndex * 4) + 3], QuantilizedRadiance.w);
                InterlockedAdd(SharedReprojectedSampleCounts[ReprojectedProbeTexelIndex], 1);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Calculate the radiance backup value to be used for unvisited cells
    float4 SumBackupRadiance = 0;
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_SIZE * TILE_SIZE; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        float4 Radiance = float4(RecoverRadiance(uint3(SharedReprojectedRadiance[(TexelIndex * 4) + 0],
                                              SharedReprojectedRadiance[(TexelIndex * 4) + 1],
                                              SharedReprojectedRadiance[(TexelIndex * 4) + 2])),
                                              SharedReprojectedSampleCounts[TexelIndex] > 0 ? 1 : 0);
        SumBackupRadiance += Radiance;
    }
    SumBackupRadiance = WaveActiveSum(SumBackupRadiance);

    // 1000 is the magic hit distance for the backup radiance
    float4 BackupRadianceDepth = float4(SumBackupRadiance.xyz / max(SumBackupRadiance.w, 1.0f), 1000);
    if(false) {
        float  EmptyTexelCount = TILE_TEXEL_COUNT - SumBackupRadiance.w;
        // The division is just a hack to make the backup radiance 
        BackupRadianceDepth.xyz = BackupRadianceDepth.xyz / max(EmptyTexelCount, 1.0f);
    }

    // No probe found, this is a disocclusion
    if (IsInvalid(MinScreenProbeScore))
    {
        if (WaveIsFirstLane())
        {
            // Mark this probe as invalid
            RWTileScreenProbeHeaderTexture[GroupID] = INVALID_UINT;
            // Add the tile to the list of projection fail list (prioritized for spawning new probes)
            uint ReprojectionFailListIndex = 0;
            InterlockedAdd(RWReprojectionFailTileCountBuffer[0], 1, ReprojectionFailListIndex);
            RWReprojectionFailTileListBuffer[ReprojectionFailListIndex] = PackUint2x16(GroupID);
        }

        return; // reprojection failed :'(
    }

    // Inject the probe index
    if(WaveIsFirstLane()) {
        ScreenProbeHeader SelectedProbeHeader = (ScreenProbeHeader)0;
        SelectedProbeHeader.PixelCoords = SelectedPixelIndex;
        SelectedProbeHeader.bValid = true;
        RWTileScreenProbeHeaderTexture[GroupID] = PackProbeHeader(SelectedProbeHeader);
    }


    // And reproject the radiance
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_SIZE * TILE_SIZE; BaseTexelIndex += WAVE_SIZE) {
        uint TileTexelIndex = BaseTexelIndex + LocalID;
        float4 RadianceDepth = RecoverRadiance(uint4(SharedReprojectedRadiance[TileTexelIndex * 4 + 0],
                                                    SharedReprojectedRadiance[TileTexelIndex * 4 + 1],
                                                    SharedReprojectedRadiance[TileTexelIndex * 4 + 2],
                                                    SharedReprojectedRadiance[TileTexelIndex * 4 + 3]));

        uint SampleCount = SharedReprojectedSampleCounts[TileTexelIndex];

        if (SampleCount > 0) RadianceDepth /= SampleCount;
        else RadianceDepth = BackupRadianceDepth;
        uint2 TileIndex = GroupID * TILE_SIZE + uint2(TileTexelIndex % TILE_SIZE, TileTexelIndex / TILE_SIZE);
        RWScreenProbeRadianceDepthTexture[TileTexelIndex] = RadianceDepth;
    }
}

// Reproject the cached probes and allocate storages (probes that are not not on screen but catched by tile LRU buffers in the previous frame)
[numthread(WAVE_SIZE, 1, 1)]
void ReprojectCachedProbes (uint DispatchID : SV_DispatchThreadID) {
    uint MRUEntryIndex = DispatchID;
    if(MRUEntryIndex >= PreviousScreenProbeCacheEntryCount[0]) return;

    uint CacheEntryIndex = PreviousTileScreenProbeCacheListIndexBuffer[MRUEntryIndex];
    float4 CacheEntryData = PreviousScreenProbeCacheDataBuffer[CacheEntryIndex];
    float3 ProbeWorldPosition = CacheEntryData.xyz;
    uint PackedProbeHeader = CacheEntryData.w;
    ScreenProbeHeader ProbeHeader = UnpackProbeHeader(PackedProbeHeader);

    CameraParameters C = GetActiveCamera();

    if (ProbeHeader.bValid)
    {
        float3 NDC = TransformPoint(C.WorldToNDC, ProbeWorldPosition);
        float2 UV  = NDC2ToUV(NDC.xy);
        if (all(UV > 0.0f) && all(UV < 1.0f))
        {
            uint2 ReprojectedTileIndex = UV * UB.TileDimensions;
            uint2 ReprojectedScreenCoords = UV * C.FilmDimensions;
            uint TileIndex1 = ReprojectedTileIndex.x + ReprojectedTileIndex.y * UB.TileDimensions.x;
            // Append to the tile's MRU list
            uint TileEntryIndex;
            InterlockedAdd(RWTileScreenProbeCacheIndexListLengthsBuffer[TileIndex1], 1, TileEntryIndex);

            // To the global reprojection entry list
            uint ReprojectionEntryIndex;
            InterlockedAdd(RWScreenProbeCacheIndexReprojectionCountBuffer[0], 1, ReprojectionEntryIndex);

            RWScreenProbeCacheIndexReprojectionEntryBuffer[ReprojectionEntryIndex] = uint4(TileEntryIndex, PackUint2x16(ReprojectedScreenCoords), CacheEntryIndex, 0);
        }
    }
}

[numthreads(WAVE_SIZE, 1, 1)]
void AllocateTileScreenProbeMRULists (uint DispatchID : SV_DispatchThreadID) {
    uint TileIndex1 = DispatchID;
    if(TileIndex1 >= UB.TileCount) return;

    uint TileEntryBase;
    InterlockedAdd(RWScreenProbeTileIndexListAllocator[0], RWTileScreenProbeCacheIndexListLengthsBuffer[TileIndex1], TileEntryBase);
    RWTileScreenProbeCacheIndexListOffsetsBuffer[TileIndex1] = TileEntryBase;
}

// Scatter the reprojected probes to finish the reprojected cached probe list index for each tile
[numthreads(WAVE_SIZE, 1, 1)]
void ScatterReprojectedCachedProbesToMRUList (uint DispatchID : SV_DispatchThreadID) {
    uint ReprojectionEntryIndex = DispatchID;
    if(ReprojectionEntryIndex >= RWScreenProbeCacheIndexReprojectionCountBuffer[0]) return;
    uint4 ReprojectionEntry = RWScreenProbeCacheIndexReprojectionEntryBuffer[ReprojectionEntryIndex];
    uint2 ProbeScreenCoords = UnpackUint2x16(ReprojectionEntry.y);
    uint2 TileIndex = ProbeScreenCoords / TILE_SIZE;
    uint  TileIndex1 = TileIndex.x + TileIndex.y * UB.TileDimensions.x;
    uint TileEntryBase = RWTileScreenProbeCacheIndexListOffsetsBuffer[TileIndex1];
    uint TileEntryIndex = ReprojectionEntry.x;
    uint PreviousCacheEntryIndex = ReprojectionEntry.z;
    RWTileScreenProbeCacheIndexListBuffer[TileEntryBase + TileEntryIndex] = PreviousCacheEntryIndex;
}

bool ShouldSpawnProbe (uint2 TileIndex) {
    // Interleaved spawning
    return ((TileIndex.x + TileIndex.y) % 2) == UB.FrameIndex % 2;
}

// Spawn a fraction of new probes for interleaved tiles each frame
[numthreads(WAVE_SIZE, 1, 1)]
void SpawnScreenProbes (uint DispatchID : SV_DispatchThreadID) {
    uint TileIndex1 = DispatchID;
    if(TileIndex1 >= UB.TileCount) return;
    uint2 TileIndex = uint2(TileIndex1 % UB.TileDimensions.x, TileIndex1 / UB.TileDimensions.x);

    // Spawn one probe per tile. This can be adjusted to spawn interleaved probes.
    if(ShouldSpawnProbe(TileIndex)) {
        CameraParameters C = GetActiveCamera();
        uint2 SubTileJitter = min(CalculateHaltonSequence(UB.FrameIndex) * TILE_SIZE, TILE_SIZE - 1.0f);
        uint2 ScreenCoords  = min(TileIndex * TILE_SIZE + SubTileJitter, C.FilmDimensions - 1);
        float2 UV = ScreenCoords * C.InvFilmDimensions;
        float ReversedZDepth       = G_Depth.SampleLevel(PointEdgeSampler, UV, 0).x;
        
        if (ReversedZDepth > 0)
        {
            uint SpawnListWaveRank = 0;
            SpawnListWaveRank = WavePrefixCountBits(1);
            uint SpawnListWaveSum = WaveActiveCountBits(1);
            uint SpawnListWaveBaseRank = 0;
            if(WaveIsFirstLane()) {
                InterlockedAdd(RWScreenProbeSpawnCountBuffer[0], 1, SpawnListWaveSum);
            }
            SpawnListWaveBaseRank = WaveReadLaneFirst(SpawnListWaveSum);
            uint SpawnListRank = SpawnListWaveBaseRank + SpawnListWaveRank;
            ScreenProbeHeader Header = (ScreenProbeHeader)0;
            Header.bValid = true;
            Header.PixelCoords = ScreenCoords;
            RWScreenProbeSpawnListBuffer[SpawnListRank] = PackProbeHeader(Header);
        }
    }
}

// Prioritize the spawnning of probes in reprojection fail tiles by substituting them into the spawn list
// Patch holes for reprojection dissoclusions 
[numthreads(WAVE_SIZE, 1, 1)]
void SubstituteScreenProbes (uint DispatchID : SV_DispatchThreadID) {
    uint FailListIndex = DispatchID;
    uint FailTileCount = RWReprojectionFailTileCountBuffer[0];
    if(FailListIndex >= FailTileCount) return;
    uint2 FailTileIndex = UnpackUint2x16(RWReprojectionFailTileListBuffer[FailListIndex]);
    uint SpawnProbeCount = min(RWScreenProbeSpawnCountBuffer[0], UB.MaxProbesToSpawnPerFrame);
    uint2 SubTileJitter = min(CalculateHaltonSequence(UB.FrameIndex) * TILE_SIZE, TILE_SIZE - 1.0f);
    ScreenProbeHeader Header = (ScreenProbeHeader)0;
    Header.bValid = true;
    Header.PixelCoords = FailTileIndex * TILE_SIZE + SubTileJitter;
    uint PackedHeader = PackProbeHeader(Header);
    // Firstly try to append to the tail of the spawn list
    if(SpawnProbeCount + FailListIndex < UB.MaxProbesToSpawnPerFrame) {
        // Write the header to the spawn list
        RWScreenProbeSpawnListBuffer[SpawnProbeCount + FailListIndex] = PackedHeader;
    } else {
        // Okay, we have too many probes to spawn, so we'll just substitute a previously spawned probe for update
        // Firstly compute the number of elements to substitute
        uint SubstituteElementCount = SpawnProbeCount + FailListIndex - UB.MaxProbesToSpawnPerFrame;
        // The element we're going to substitute
        uint SubstituteElementIndex = FailListIndex - SubstituteElementCount;
        if(SubstituteElementIndex < UB.MaxProbesToSpawnPerFrame) {
            if(SubstituteElementCount < SpawnProbeCount) {
                // Pick a random probe from the suffix spawn list to substitute
                // Making our substitutions distribute more evenly among the spawn list
                Random rng = MakeRandom(DispatchID, UB.FrameIndex);
                uint RandomElementIndex = SubstituteElementCount + rng.rand() * max(0, SpawnProbeCount - SubstituteElementCount);
                // If the random element is valid, then swap it with the substitute element to 
                // scatter the substitutions
                if(RandomElementIndex < SpawnProbeCount) {
                    uint OriginalValue = 0;
                    InterlockedExchange(
                        RWScreenProbeSpawnListBuffer[RandomElementIndex],
                        RWScreenProbeSpawnListBuffer[SubstituteElementIndex],
                        OriginalValue
                    );
                }
                RWScreenProbeSpawnListBuffer[SubstituteElementIndex] = PackedHeader;
            }
        }
    }
}


float RadianceToSampleWeight (float3 Radiance) {
    return RadianceToLuminance(Radiance) + 1e-5f;
}

groupshared uint4 SharedProbeBlendedRadiance[TILE_SIZE * TILE_SIZE];
groupshared float SharedProbeOctahedronSampleWeight[TILE_SIZE * TILE_SIZE];
groupshared float SharedProbeOctahedronSampleWeightPrefixSum[TILE_SIZE * TILE_SIZE];
// For each probe to be spawned, reproject history & sample update rays and locate probe cache entries to update/evict
[numthreads(WAVE_SIZE, 1, 1)]
void ReprojectHistory_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries (uint GroupID : SV_GroupID, uint LocalID : SV_LocalID) {
    uint SpawnListIndex = GroupID;
    uint SpawnListCount = min(
        RWScreenProbeSpawnCountBuffer[0] + RWReprojectionFailTileCountBuffer[0],
        UB.MaxProbesToSpawnPerFrame
    );
    if(SpawnListIndex >= SpawnListCount) return;

    ScreenProbeHeader Header = UnpackProbeHeader(RWScreenProbeSpawnListBuffer[SpawnListIndex]);
    uint2 TileIndex = Header.PixelCoords / TILE_SIZE;
    for(int BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        SharedProbeBlendedRadiance[TexelIndex] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    
    CameraParameters C = GetActiveCamera();
    

    // Properties of the newly spawned probe
    float2 UV = (Header.PixelCoords + 0.5f) * C.InvFilmDimensions;
    float LinearDepth = ReversedZDepthToLinearDepth(C, G_Depth.SampleLevel(PointEdgeSampler, UV, 0).x);
    float3 WorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(UV), LinearDepth);
    float3 Normal = normalize(G_Normal.SampleLevel(PointEdgeSampler, UV, 0) * 2 - 1);

    // Properties of the previous probe reprojected from last frame (if any) in the same tile
    uint PackedPreviousProbeHeader = PreviousTileScreenProbeHeaderTexture[TileIndex];
    ScreenProbeHeader PreviousProbe = UnpackProbeHeader(PackedPreviousProbeHeader);
    float3 PreviousProbeWorldPos = 0;
    float3 PreviousProbeNormal = 0;
    if(PreviousProbe.bValid) {
        float2 PreviousProbeUV = (PreviousProbe.PixelCoords + 0.5f) * C.InvFilmDimensions;
        float PreviousProbeLinearDepth = ReversedZDepthToLinearDepth(C, G_Depth.SampleLevel(PointEdgeSampler, PreviousProbeUV, 0).x);
        PreviousProbeWorldPos = RecoverWorldPositionNDC2(C, UVToNDC2(PreviousProbeUV), PreviousProbeLinearDepth);
        PreviousProbeNormal = normalize(G_Normal.SampleLevel(PointEdgeSampler, PreviousProbeUV, 0).rgb * 2 - 1);
    }


    float SearchSize = LinearDepth * UB.ProbeReprojectionSearchSize;
    uint  ReusedProbeTexelCount = 0;

    // Recover radiance at the new probe from reprojected probes on neighbor tiles
    for(int dx = -1; dx <= 1; dx++) {
        for(int dy = -1; dy <= 1; dy++) {
            int2 NeighborTileIndex = TileIndex + int2(dx, dy);
            if(any(NeighborTileIndex < 0) || any(NeighborTileIndex >= UB.TileDimensions)) continue ;
            ScreenProbeHeader NeighborHeader = UnpackProbeHeader(RWTileScreenProbeHeaderTexture[NeighborTileIndex]);
            if(!NeighborHeader.bValid) continue ;
            uint2 NeighborProbeUV = (NeighborHeader.PixelCoords + 0.5f) * C.InvFilmDimensions;
            float NeighbotProbeLinearDepth = ReversedZDepthToLinearDepth(C, G_Depth.SampleLevel(PointEdgeSampler, NeighborProbeUV, 0).x);
            float3 NeighborProbeWorldPos = RecoverWorldPositionNDC2(C, UVToNDC2(NeighborProbeUV), NeighbotProbeLinearDepth);
            if(abs(dot(NeighborProbeWorldPos - WorldPosition, Normal)) > SearchSize) continue ;
            float3 NeightborProbeNormal = normalize(G_Normal.SampleLevel(PointEdgeSampler, NeighborProbeUV, 0).rgb * 2 - 1);
            float3 NeighborProbeTangent, NeighborProbeBitangent;
            GetOrthoVectors(NeightborProbeNormal, NeighborProbeTangent, NeighborProbeBitangent);
            for(int BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
                uint TexelIndex = BaseTexelIndex + LocalID;
                uint2 ProbeTexelCoords = int2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
                uint2 AtlasTexelCoords = NeighborTileIndex * TILE_SIZE + ProbeTexelCoords;
                float4 ProbeRadianceDepth = RWScreenProbeRadianceDepthTexture[AtlasTexelCoords];
                float2 ProbeTexelUV = (ProbeTexelCoords + 0.5f) / TILE_SIZE;
                float3 ProbeLocalDirection = HemiOctahedron01ToUnitVectorA(ProbeTexelUV);
                float3 ProbeWorldDirection = ProbeLocalDirection.x * NeighborProbeTangent + ProbeLocalDirection.y * NeighborProbeBitangent + ProbeLocalDirection.z * NeightborProbeNormal;
                float3 HitPosition = NeighborProbeWorldPos + ProbeWorldDirection * ProbeRadianceDepth.w;
                float3 ReprojectedDirection = HitPosition - WorldPosition;
                if(dot(Normal, ReprojectedDirection) > 1e-4f) {
                    float3 ReprojectedWorldDirection = normalize(ReprojectedDirection);
                    float2 ReprojectedTexelUV = UnitVectorToHemiOctahedron01A(ReprojectedWorldDirection);
                    uint2 ReprojectedTexelCoords = uint2(ReprojectedTexelUV * TILE_SIZE);
                    uint ReprojectedTexelIndex = ReprojectedTexelCoords.x + ReprojectedTexelCoords.y * TILE_SIZE;
                    InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].x, QuantilizeRadiance(ProbeRadianceDepth.x));
                    InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].y, QuantilizeRadiance(ProbeRadianceDepth.y));
                    InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].z, QuantilizeRadiance(ProbeRadianceDepth.z));
                    InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].w, QuantilizeRadiance(ProbeRadianceDepth.w));
                    ReusedProbeTexelCount ++;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // Try to recover radiance at the new probe from cached probes in nearby tiles
    // As well as finding the closest probe to update at the same time in the cache
    float MinUpdateProbeScoreFromCache = 1e+10f;
    uint  MinUpdateProbeScoreCacheEntryIndex = INVALID_UINT;
    // And finding a possible matching cache entry for the existing old probe to evict
    float MinPreviousProbeScoreFromCache = 1e+10f;
    uint  MinPreviousProbeScoreCacheEntryIndex = INVALID_UINT;
    for(int dx = -1; dx <= 1; dx++) {
        for(int dy = -1; dy <= 1; dy++) {
            int2 NeighborTileIndex = TileIndex + int2(dx, dy);
            if(any(NeighborTileIndex < 0) || any(NeighborTileIndex >= UB.TileDimensions)) continue ;
            uint NeighborTileIndex1 = NeighborTileIndex.x + NeighborTileIndex.y * UB.TileDimensions.x;
            uint TileMRUListLenght = RWTileScreenProbeCacheIndexListLengthsBuffer[NeighborTileIndex1];
            uint TileMRUListOffset = RWTileScreenProbeCacheIndexListOffsetsBuffer[NeighborTileIndex1];
            for(uint ProbeMRUListRank = 0; ProbeMRUListRank < TileMRUListLenght; ProbeMRUListRank++) {
                uint MRUListIndex = TileMRUListOffset + ProbeMRUListRank;
                uint CacheEntryIndex = RWScreenProbeCacheDataBuffer[MRUListIndex];
                float4 CacheEntry = PreviousScreenProbeCacheDataBuffer[CacheEntryIndex];
                float3 CachedProbeWorldPos = CacheEntry.xyz;
                float3 CachedProbeNormal = UnpackNormal(CacheEntry.w);
                if(abs(dot(CachedProbeWorldPos - WorldPosition, Normal)) < SearchSize) {
                    float3 CachedProbeTangent, CachedProbeBitangent;
                    GetOrthoVectors(CachedProbeNormal, CachedProbeTangent, CachedProbeBitangent);
                    for(int BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
                        uint TexelIndex = BaseTexelIndex + LocalID;
                        uint2 ProbeTexelCoords = int2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
                        uint2 AtlasTexelCoords = NeighborTileIndex * TILE_SIZE + ProbeTexelCoords;
                        float4 ProbeRadianceDepth = RWScreenProbeRadianceDepthTexture[AtlasTexelCoords];
                        float2 ProbeTexelUV = (ProbeTexelCoords + 0.5f) / TILE_SIZE;
                        float3 ProbeLocalDirection = HemiOctahedron01ToUnitVectorA(ProbeTexelUV);
                        float3 ProbeWorldDirection = ProbeLocalDirection.x * CachedProbeTangent + ProbeLocalDirection.y * CachedProbeBitangent + ProbeLocalDirection.z * CachedProbeNormal;
                        float3 HitPosition = CachedProbeWorldPos + ProbeWorldDirection * ProbeRadianceDepth.w;
                        float3 ReprojectedDirection = HitPosition - WorldPosition;
                        if(dot(Normal, ReprojectedDirection) > 1e-4f) {
                            float3 ReprojectedWorldDirection = normalize(ReprojectedDirection);
                            float2 ReprojectedTexelUV = UnitVectorToHemiOctahedron01A(ReprojectedWorldDirection);
                            uint2 ReprojectedTexelCoords = uint2(ReprojectedTexelUV * TILE_SIZE);
                            uint ReprojectedTexelIndex = ReprojectedTexelCoords.x + ReprojectedTexelCoords.y * TILE_SIZE;
                            InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].x, QuantilizeRadiance(ProbeRadianceDepth.x));
                            InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].y, QuantilizeRadiance(ProbeRadianceDepth.y));
                            InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].z, QuantilizeRadiance(ProbeRadianceDepth.z));
                            InterlockedAdd(SharedProbeBlendedRadiance[ReprojectedTexelIndex].w, QuantilizeRadiance(ProbeRadianceDepth.w));
                            ReusedProbeTexelCount ++;
                        }
                    }
                    float ProbeScore = distance(CachedProbeWorldPos, WorldPosition);
                    if(dot(Normal, CachedProbeNormal) >= 0.95f && ProbeScore < MinUpdateProbeScoreFromCache) {
                        MinUpdateProbeScoreFromCache = ProbeScore;
                        MinUpdateProbeScoreCacheEntryIndex = CacheEntryIndex;
                    }
                }
                if(abs(dot(CachedProbeWorldPos - PreviousProbeWorldPos, Normal)) < SearchSize && PreviousProbe.bValid) {
                    float ProbeScore = distance(CachedProbeWorldPos, PreviousProbeWorldPos);
                    if(dot(PreviousProbeNormal, CachedProbeNormal) >= 0.95f && ProbeScore < MinPreviousProbeScoreFromCache) {
                        MinPreviousProbeScoreFromCache = ProbeScore;
                        MinPreviousProbeScoreCacheEntryIndex = CacheEntryIndex;
                    }
                }
            }
        }
    }
    // Write out the previous probe to evict (if any), cache to substitute & update
    {
        RWScreenProbeSpawnCacheMatchesBuffer[SpawnListIndex] = uint2(MinPreviousProbeScoreCacheEntryIndex, MinUpdateProbeScoreCacheEntryIndex);
    }
    // Sample rays
    // We assume that ray count is always a multiple of WAVE_SIZE
    // Prepare probe local coordinate system
    float3 ProbeTangent, ProbeBitangent;
    GetOrthoVectors(Normal, ProbeTangent, ProbeBitangent);

    const float Epsilon = 1e-5f;

    float SumSizeOctahedronOriginal = 0.f;
    // Load octahedron sample weights
    {
        float SavedPrefixSum = 0.f;
        for(int BaseProbeTexelIndex = 0; BaseProbeTexelIndex < TILE_TEXEL_COUNT; BaseProbeTexelIndex += WAVE_SIZE) {
            int  ProbeTexelIndex = BaseProbeTexelIndex + LocalID;
            int2 ProbeTexelCoords = int2(ProbeTexelIndex % TILE_SIZE, ProbeTexelIndex / TILE_SIZE);
            float4 RadianceDepth = RecoverRadiance(uint4(SharedProbeBlendedRadiance[ProbeTexelIndex].x,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].y,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].z,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].w));
            float3 Radiance = RadianceDepth.xyz;
            float AreaCorrectionFactor = 1.f;
            float  SampleWeight = RadianceToSampleWeight(Radiance) * AreaCorrectionFactor;
            SharedProbeOctahedronSampleWeight[ProbeTexelIndex] = SampleWeight;
            float PrefixSum = WavePrefixSum(SampleWeight) + SavedPrefixSum;
            SharedProbeOctahedronSampleWeightPrefixSum[ProbeTexelIndex] = PrefixSum;
#if TILE_TEXEL_COUNT % WAVE_SIZE != 0
#error "TILE_TEXEL_COUNT must be a multiple of WAVE_SIZE"
#endif
            SavedPrefixSum = WaveReadLaneAt(PrefixSum + RadianceToSampleWeight(Radiance), WAVE_SIZE - 1); 
        }
        SumSizeOctahedronOriginal = SavedPrefixSum;
    }
    GroupMemoryBarrierWithGroupSync();
    // Assume that octahedron mapping to sphere is area preserving (ignoring the distortion)
    float SumSizeOctahedron = SumSizeOctahedronOriginal * (TWO_PI / TILE_TEXEL_COUNT) + Epsilon;
    float SumSize = SumSizeOctahedron;

    Random rng = MakeRandom((GroupID * WAVE_SIZE + LocalID) ^ 0x718f3a21u, UB.ProbeUpdateRaySampleSeed);
#if MAX_NUM_UPDATE_RAYS_PER_PROBE % WAVE_SIZE != 0
#error "MAX_NUM_UPDATE_RAYS_PER_PROBE must be a multiple of WAVE_SIZE"
#endif
    // Allocate a number of rays to sample the probe octahedron
    int NumProbeOctahedronSamples = TILE_TEXEL_COUNT;
    // Round to a multiple of WAVE_SIZE with russian roulette for maximum wave coherence & occupancy
    {
        float P = saturate(1 - float(ReusedProbeTexelCount) / 128);
        // For the case that we have no history for reuse, double the number of samples
        if(rng.rand() < P) {
            NumProbeOctahedronSamples += TILE_TEXEL_COUNT;
        }
    }
    uint UpdateRayIndexBase = 0;
    if(WaveIsFirstLane()) {
        RWScreenProbeUpdateRayCountsBuffer[SpawnListIndex] = NumProbeOctahedronSamples;
        InterlockedAdd(RWScreenProbeUpdateRayAllocator[SpawnListIndex], NumProbeOctahedronSamples, UpdateRayIndexBase);
    }
    UpdateRayIndexBase = WaveReadLaneFirst(UpdateRayIndexBase);

    // Sample probe octahedron
    [unroll(MAX_NUM_UPDATE_RAYS_PER_PROBE / WAVE_SIZE)]
    for(int RayRankBase = 0; RayRankBase < NumProbeOctahedronSamples; RayRankBase += WAVE_SIZE) {
        // We assume that ray count is always a multiple of WAVE_SIZE
        int RayRank = RayRankBase + LocalID;
        float  u    = rng.rand();
        float2 u2   = rng.rand2();
		float  u3   = rng.rand();
        float  U    = u * SumSizeOctahedronOriginal;
        int L = 0, R = TILE_TEXEL_COUNT;
        for(int i = 0; i < TILE_TEXEL_COUNT_L2; i++) {
            int Mid = (L + R) / 2;
            if(SharedProbeOctahedronSampleWeightPrefixSum[Mid] < U) L = Mid;
            else R = Mid;
        }
        float OctPdf = SharedProbeOctahedronSampleWeight[L] / max(SumSizeOctahedronOriginal, Epsilon) * TILE_TEXEL_COUNT;
        float2 TexelUV = u2;
        int2   TexelCoords = int2(L % TILE_SIZE, L / TILE_SIZE);
        float2 OctahedronUV = (TexelUV + TexelCoords) * (1.f / TILE_SIZE);
        float3 RayDirection = HemiOctahedron01ToUnitVectorA(OctahedronUV);
        RayDirection = RayDirection.x * ProbeTangent + RayDirection.y * ProbeBitangent + RayDirection.z * Normal;
        // Convert from [0, 1]^2 to S^2
        float AreaCorrectionFactor = 1.f;
        OctPdf = OctPdf * AreaCorrectionFactor * (1.f / TWO_PI);

        float RayPdf = OctPdf;
        if(UB.ProbeUpdateRaysNoImportanceSampling) {
            RayPdf = UniformSampleHemispherePdf();
            RayDirection = UniformSampleHemisphere(u2);
            RayDirection = normalize(ProbeTangent * RayDirection.x + ProbeBitangent * RayDirection.y + Normal * RayDirection.z);
        }
        // 24.06.23: This must be checked otherwise there're precision issues
        float bValid = dot(RayDirection, Normal) > 0;
        uint RayIndex = RayRank + UpdateRayIndexBase;
		// Setup ray to trace indirection list (for later compound tracing)
#define MIN_PDF_TO_TRACE 1e-3f
		if(RayPdf >= MIN_PDF_TO_TRACE && bValid) {
			// A valid update ray is spawned
			// Queue up for a ray trace
            RWScreenProbeUpdateRayDirectionBuffer[RayIndex] = PackNormal(RayDirection);
			RWScreenProbeUpdateRayResultBuffer[RayIndex] = PackFp16x4Safe(0.f.xxxx);
			// Keep extra data for later probe update
			float RayInvPdf = 1.f / RayPdf;
            RWScreenProbeUpdateRayInvPdfBuffer[RayIndex] = RayInvPdf;
        } else {
            // Invalid update ray, mark it as invalid to skip tracing
            RWScreenProbeUpdateRayResultBuffer[RayIndex] = PackFp16x4Safe(0.f.xxxx);
            RWScreenProbeUpdateRayInvPdfBuffer[RayIndex] = 0.f;
            RWScreenProbeUpdateRayDirectionBuffer[RayIndex] = PackNormal(0.f.xxx);
        }
    }
}

// Trace...

[numthreasds(WAVE_SIZE, 1, 1)]
void AgeCache (uint DispatchID : SV_DispatchThreadID) {
    uint2 TileIndex = uint2(GroupID.x / TILE_SIZE, GroupID.x / TILE_SIZE);
}

// Update screen probes & cache
groupshared uint SharedProbeSampleCounts[TILE_SIZE * TILE_SIZE];
[numthreads(WAVE_SIZE, 1, 1)]
void UpdateScreenProbesAndCache (uint GroupID : SV_GroupID, uint LocalID : SV_LocalThreadID) {
    uint SpawnListIndex = GroupID;
    uint SpawnListCount = min(
        RWScreenProbeSpawnCountBuffer[0] + RWReprojectionFailTileCountBuffer[0],
        UB.MaxProbesToSpawnPerFrame
    );
    if(SpawnListIndex >= SpawnListCount) return;

    // Clear the shared memory for later ray radiance accumulation
    ScreenProbeHeader Header = UnpackProbeHeader(RWScreenProbeSpawnListBuffer[SpawnListIndex]);
    uint2 TileIndex = Header.PixelCoords / TILE_SIZE;
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        SharedProbeBlendedRadiance[TexelIndex] = 0;
        SharedProbeSampleCounts[TexelIndex] = 0;
    }
    uint PackedPreviousProbeHeader = RWTileScreenProbeHeaderTexture[TileIndex];
    bool bPreviousProbeValid = UnpackProbeHeader(PackedPreviousProbeHeader).bValid;
    uint2 PreviousProbeIndex = UnpackProbeHeader(PackedPreviousProbeHeader).PixelCoords / TILE_SIZE;
    GroupMemoryBarrierWithGroupSync();

    float SumRayWeight = 0;
    float4 SumRayResult = 0;

    // Accumulate radiance from traced update rays
    uint ProbeUpdateRayBase = RWScreenProbeUpdateRayOffsetsBuffer[SpawnListIndex];
    for(uint BaseRayRank = 0; BaseRayRank < TILE_TEXEL_COUNT; BaseRayRank += WAVE_SIZE) {
        uint RayRank = BaseRayRank + LocalID;
        uint RayIndex = ProbeUpdateRayBase + RayRank;
        float4 RayResult = UnpackFp16x4Safe(RWScreenProbeUpdateRayResultBuffer[RayIndex]);
        bool bValid = RayResult.w > 0;
        if(bValid) {
            float3 RayDirection = UnpackNormal(RWScreenProbeUpdateRayDirectionBuffer[RayIndex]);
            float RayInvPdf = RWScreenProbeUpdateRayInvPdfBuffer[RayIndex];
            float3 RayRadiance = RayResult.xyz;
            float2 RayOctahedronUV = UnitVectorToHemiOctahedron01A(RayDirection);
            uint2 RayTexelCoords = uint2(RayOctahedronUV * TILE_SIZE);
            uint RayTexelIndex = RayTexelCoords.x + RayTexelCoords.y * TILE_SIZE;
            InterlockedAdd(SharedProbeBlendedRadiance[RayTexelIndex].x, QuantilizeRadiance(RayRadiance.x));
            InterlockedAdd(SharedProbeBlendedRadiance[RayTexelIndex].y, QuantilizeRadiance(RayRadiance.y));
            InterlockedAdd(SharedProbeBlendedRadiance[RayTexelIndex].z, QuantilizeRadiance(RayRadiance.z));
            InterlockedAdd(SharedProbeBlendedRadiance[RayTexelIndex].w, QuantilizeRadiance(RayResult.w));
            InterlockedAdd(SharedProbeSampleCounts[RayTexelIndex], 1);
            SumRayWeight += RayInvPdf;
            SumRayResult += RayResult * RayInvPdf;
        }
    }

    SumRayWeight = WaveActiveSum(SumRayWeight);
    SumRayResult = WaveActiveSum(SumRayResult);
    float4 AverageRayResult = SumRayResult / max(SumRayWeight, 1e-5f);

    GroupMemoryBarrierWithGroupSync();

    float4 BackupRayResult = AverageRayResult;
    // Decode new radiance values and update

    uint2 CacheMatches = RWScreenProbeSpawnCacheMatchesBuffer[SpawnListIndex];
    uint ProbeToEvictCacheEntryIndex = CacheMatches.x;
    uint ProbeToUpdateCacheEntryIndex = CacheMatches.y;

    if(bPreviousProbeValid && IsInvalid(ProbeToEvictCacheEntryIndex)) {
        // The previous probe have to be allocated a new cache entry
        // Try to allocate a new cache entry
        uint FreeListIndex, CacheEntryIndex = INVALID_UINT;
        if(WaveIsFirstLane()) {
            InterlockedAdd(RWScreenProbeCacheFreeListAllocator[0], -1, FreeListIndex);
            // FreeListIndex naturally becomes a big number if the cache is full
            if(FreeListIndex < UB.TileCount) {
                CacheEntryIndex = RWScreenProbeCacheFreeListBuffer[FreeListIndex];
                // Write metadata
                RWScreenProbeCacheTimestampBuffer[CacheEntryIndex] = UB.FrameIndex;
                RWScreenProbeCacheDataBuffer[CacheEntryIndex] 
                    = float4(PreviousProbeWorldPos, asfloat(PackNormal(PreviousProbeNormal)));
            }
        }
        CacheEntryIndex = WaveReadLaneFirst(CacheEntryIndex);
        if(IsValid(CacheEntryIndex)) {
            // Allocated cache entry to be written to later.
            ProbeToEvictCacheEntryIndex = CacheEntryIndex;
        }
    }

    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        uint2 TexelCoords = uint2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
        uint2 AtlasTexelCoords = TileIndex * TILE_SIZE + TexelCoords;
        uint4 NewRadianceQuantized = SharedProbeBlendedRadiance[TexelIndex];
        uint   SampleCount = SharedProbeSampleCounts[TexelIndex];
        float4 NewRadiance = RecoverRadiance(NewRadianceQuantized) / max(SampleCount, 1);
        if(SampleCount == 0) {
            NewRadiance = BackupRayResult;
        }
        if (bPreviousProbeValid)
        {
            float4 PreviousRadiance = PreviousScreenProbeRadianceDepthTexture.Load(int3(AtlasTexelCoords, 0));
            float lumaA = RadianceToLuminance(NewRadiance.xyz);
            float lumaB = RadianceToLuminance(PreviousRadiance.xyz);

            // Shadow-preserving biased temporal hysteresis (inspired by: https://www.youtube.com/watch?v=WzpLWzGvFK4&t=630s)
            float temporal_blend = Squared(clamp(max(lumaA - lumaB - min(lumaA, lumaB), 0.0f) / max(max(lumaA, lumaB), 1e-4f), 0.0f, 0.95f));

            NewRadiance = lerp(NewRadiance, PreviousRadiance, temporal_blend);
        }

        if(bPreviousProbeValid) {
            if(ProbeToEvictCacheEntryIndex != INVALID_UINT) {
                // Have to evict a cache entry (replaced by the previous probe)
                uint2 CacheAtlasIndex = uint2(
                    ProbeToEvictCacheEntryIndex % UB.TileDimensions.x,
                    ProbeToEvictCacheEntryIndex / UB.TileDimensions.x
                );
                uint2 CacheAtlasBaseCoords = CacheAtlasIndex * TILE_SIZE;
                uint2 ProbeAtlasBaseCoords = PreviousProbeIndex * TILE_SIZE;
                // Copy the previous probe to the cache entry
                uint2 CacheAtlasTexelCoords = CacheAtlasBaseCoords + TexelCoords;
                uint2 ProbeAtlasTexelCoords = ProbeAtlasBaseCoords + TexelCoords;
                RWScreenProbeCacheRadianceDepthBuffer[CacheAtlasTexelCoords] = 
                    RWScreenProbeRadianceDepthTexture[ProbeAtlasTexelCoords];
            } else {
                // This should never happen if cache has not been overflown
            }
        }

        if(ProbeToUpdateCacheEntryIndex != INVALID_UINT) {
            // Update an existing cache entry
            // Possibly there're multiple waves updating the same cache entry, so we have to do a CAS before updating
            asdfasdfasdf
            uint2 CacheAtlasIndex = uint2(
                ProbeToUpdateCacheEntryIndex % UB.TileDimensions.x,
                ProbeToUpdateCacheEntryIndex / UB.TileDimensions.x
            );
            uint2 CacheAtlasBaseCoords = CacheAtlasIndex * TILE_SIZE;
            uint2 CacheAtlasTexelCoords = CacheAtlasBaseCoords + TexelCoords;
            RWScreenProbeCacheRadianceDepthBuffer[CacheAtlasTexelCoords] = NewRadiance;
        }
    }
    if(WaveIsFirstLane()) {
        // Finally, update the header for indexing the newly spawned probe
        RWTileScreenProbeHeaderTexture[TileIndex] = PackProbeHeader(Header);
    }
    

}