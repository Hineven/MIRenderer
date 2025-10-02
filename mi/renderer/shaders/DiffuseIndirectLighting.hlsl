#include "headers/Conventions.hlsl"
#include "headers/Camera.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Packing.hlsl"
#include "headers/Random.hlsl"
#include "headers/OctahedronMapping.hlsl"
#include "headers/Radiometry.hlsl"
#include "headers/Sampling.hlsl"
#include "headers/SphericalHarmonics.hlsl"
#include "headers/HybridTracing.hlsl"
#include "resources/CommonSamplerResources.hlsl"

// Foreground screen probes
//[[vk::image_format(rgba16f)]]
Texture2D<float4> PreviousScreenProbeRadianceDepthTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWScreenProbeRadianceDepthTexture;

// SH projection of foreground probes
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWScreenProbeIrradianceTexture; // UB.TileDimensions
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWScreenProbeSHCoefficientsRTexture; // Doubled width ( to store 4 + 4 floats )
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWScreenProbeSHCoefficientsGTexture;
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWScreenProbeSHCoefficientsBTexture;

// Probe cache indexing and updating datastructures
RWStructuredBuffer<uint> RWTileScreenProbeCacheIndexListBuffer;
RWStructuredBuffer<uint> RWTileScreenProbeCacheIndexListLengthsBuffer;
RWStructuredBuffer<uint> RWTileScreenProbeCacheIndexListOffsetsBuffer;
RWStructuredBuffer<uint> RWTileScreenProbeCacheIndexListAllocator; // Used to allocate RWTileScreenProbeCacheIndexListBuffer entries to tiles

// Temporary buffers serving the reprojection of probe cache and rebuilding of the tile index list of cached probes.
RWStructuredBuffer<uint4> RWScreenProbeCacheIndexReprojectionEntryBuffer;
RWStructuredBuffer<uint> RWScreenProbeCacheIndexReprojectionCount;

// A temporary buffer of reconstructed radiance (when sampling update rays) for newly spawned probes. Used for temporal blending.
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWScreenProbeReconstructedRadianceDepthBuffer;

// Buffers holding the cache entries to update & evict upon probe spawning.
// Indexed with SpawnListIndex
RWStructuredBuffer<uint2> RWScreenProbeSpawnCacheMatchesBuffer;
RWStructuredBuffer<uint> RWScreenProbeCacheMRUQueueBuffer; // A MRU queue of cache entry indices.
RWStructuredBuffer<uint> RWScreenProbeCacheUpdatedMRUQueueBuffer;
RWStructuredBuffer<uint> RWScreenProbeCacheToMRUQueueIndexBuffer; // Inverse of RWScreenProbeCacheMRUQueueBuffer (cache index -> queue index)
RWStructuredBuffer<uint> RWScreenProbeCacheMRUFlagBuffer; // Mark 1 if the i-th queue element is used this frame. Also used as CAS flags for remove overlapping cache writes.
RWStructuredBuffer<uint> RWScreenProbeCacheMRUFlagPrefixSumBuffer; // Prefix sum of the above buffer
RWStructuredBuffer<uint> RWScreenProbeCacheMRUQueueEntryAllocator; // Allocate new entries (overwriting the tail elements of the MRU queue)

// Probe cache (Backup for screen probes. Evicted probes will be stored here)
RWStructuredBuffer<uint4> RWScreenProbeCacheDataBuffer; // Cached data (world position + normal)
struct CacheEntryData {
    bool bAlive;
    float3 WorldPosition;
    float3 Normal;
};

CacheEntryData UnpackCacheEntry (uint4 Data) {
    CacheEntryData Entry;
    Entry.bAlive = Data.w != 0;
    Entry.WorldPosition = asfloat(Data.xyz);
    Entry.Normal = UnpackNormal(Data.w);
    return Entry;
}

uint4 PackCacheEntry (CacheEntryData Entry) {
    uint4 Data;
    Data.xyz = asuint(Entry.WorldPosition);
    Data.w = Entry.bAlive ? PackNormal(Entry.Normal) : 0;
    return Data;
}

[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWScreenProbeCacheRadianceDepthTexture; // Cached radiance & depth

// A mipmapped texture (of resolution TileDimensions) of the probe headers in each tile
// Higher mip levels store arbitary valid probe headers in lower mips
Texture2D<uint> PreviousTileScreenProbeHeaderTexture;
// Mip 0 of the header index texture in the current frame
RWTexture2D<uint> RWTileScreenProbeHeaderTexture;
// Mipmapped version of the header index texture in the current frame
Texture2D<uint> TileScreenProbeHeaderTexture;

// The list of reprojection occlusion tiles
RWStructuredBuffer<uint> RWReprojectionFailTileCount;
RWStructuredBuffer<uint> RWReprojectionFailTileListBuffer; // uint16x2 packed tile index

// The list of new probes to spawn in this frame
RWStructuredBuffer<uint> RWScreenProbeSpawnCount;
// Packed probe headers to be spawned
RWStructuredBuffer<uint> RWScreenProbeSpawnListBuffer;

// Spawned rays
RWStructuredBuffer<uint> RWScreenProbeUpdateRayOffsetsBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayCountsBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayDirectionBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayStateBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayOriginScreenCoordsBuffer;
RWStructuredBuffer<uint> RWScreenProbeUpdateRayAllocator; // Number of all rays to be traced

// Ray trace results
RWStructuredBuffer<uint2> RWScreenProbeUpdateRayResultBuffer;
RWStructuredBuffer<float> RWScreenProbeUpdateRayInvPdfBuffer;

Texture2D<float> G_Depth;
Texture2D<float3> G_Normal;

Texture2D<float> PreviousDepthTexture;
Texture2D<float3> PreviousNormalTexture;

// Final result
[[vk::image_format("rgba16f")]]
RWTexture2D<float4> RWDiffuseIndirectLightingTexture;

struct DiffuseIndirectLightingUB {
    uint  MaxNumUpdateRays;
    uint  HeaderTileDimension;
    uint2 TileDimensions;

    float2 InvTileDimensions;
    uint  TileCount;
    float InvTileCount;

    float ProbeReprojectionSearchSize;
    uint  MaxProbesToSpawnPerFrame;
    float2 InvProbeAtlasDimensions;
    
    uint FrameIndex;
    uint ProbeUpdateRaysNoImportanceSampling;
    uint ProbeHeaderIndexMipLevelCount;
    uint ProbeUpdateRaySampleSeed;
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
    bool bTemporalBlendable;
    uint2 PixelCoords;
};

ScreenProbeHeader UnpackProbeHeader (uint PackedProbeHeader) {
    ScreenProbeHeader Header;
    Header.bValid = !(PackedProbeHeader & 0x80000000u); // Make INVALID_UINT unpacks to an invalid probe
    Header.bTemporalBlendable = (PackedProbeHeader & 0x40000000u) != 0;
    Header.PixelCoords = uint2(PackedProbeHeader & 0xFFFFu, (PackedProbeHeader >> 16) & 0x3FFFu);
    return Header;
}

uint PackProbeHeader (ScreenProbeHeader Header) {
    uint ProbeHeader = 0;
    ProbeHeader |= Header.bValid ? 0 : 0x80000000u;
    ProbeHeader |= Header.bTemporalBlendable ? 0x40000000u : 0;
    ProbeHeader |= (Header.PixelCoords.x | (Header.PixelCoords.y << 16)) & 0x3FFFFFFF;
    return ProbeHeader;
}

uint ProbeHeaderMarkTemporalBlendable (uint Packed) {
    return Packed | 0x40000000u;
}

// Quantilization
// May overflow if the radiance is too large (e.g. 1000)
uint QuantilizeRadiance (float V, float Noise = 0) {
    return floor(min(V, 1000) * 16384 + Noise);
}
uint4 QuantilizeRadiance (float4 V, float Noise = 0) {
    return uint4(
        QuantilizeRadiance(V.x, Noise), QuantilizeRadiance(V.y, Noise),
        QuantilizeRadiance(V.z, Noise), QuantilizeRadiance(V.w, Noise));
}

float RecoverRadiance (uint V) {
    return float(V) / 16384;
}
float3 RecoverRadiance (uint3 V) {
    return float3(V) / 16384;
}
float4 RecoverRadiance (uint4 V) {
    return float4(V) / 16384;
}
uint QuantilizeWeight (float V, float Noise = 0) {
    // 2^17 = 131,072 (fp32: 2^23 precision)
    // Note: InvPdf < 100, no worries about overflowing
    return floor(V * 131072.f + Noise);
}
float RecoverWeight (uint V) {
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

// Finds the closest probe to the specified location on the probe grid.
// Here, we start at the highest mip level in the probe mask and fall back
// to lower mips if failing to find a valid probe seed.
// This allows for very large probe search (up to the entire screen) very
// efficiently and is particularly useful to find the neighbor probes in
// disoccluded regions during the final radiance interpolation.
uint FindClosestScreenProbe(uint2 PixelCoords, int2 Offset = 0)
{
    uint2 TileIndex = min(PixelCoords / TILE_SIZE, UB.TileDimensions - 1);
    uint2 MipTileDimensions = UB.HeaderTileDimension;

    for (uint i = 0; i < UB.ProbeHeaderIndexMipLevelCount; ++i)
    {
        int2 Location = int2(TileIndex) + Offset;
        if(any(Location < 0) || any(Location >= int2(MipTileDimensions)))
        {
            break; // No need for further searches
        }
        uint PackedHeader = TileScreenProbeHeaderTexture.Load(int3(Location, i));
        ScreenProbeHeader Header = UnpackProbeHeader(PackedHeader);

        if (Header.bValid)
        {
            return PackedHeader;   // found a close-by probe :)
        }

        MipTileDimensions = max(MipTileDimensions >> 1, 1);

        TileIndex = min(TileIndex >> 1, MipTileDimensions - 1);
    }

    return INVALID_UINT;
}

[numthreads(1, 1, 1)]
void ClearCounters () {
    RWScreenProbeCacheIndexReprojectionCount[0] = 0;
    RWScreenProbeCacheMRUQueueEntryAllocator[0] = 0;
    RWReprojectionFailTileCount[0] = 0;
    RWScreenProbeSpawnCount[0] = 0;
    RWScreenProbeUpdateRayAllocator[0] = 0;
    RWTileScreenProbeCacheIndexListAllocator[0] = 0;
}

[numthreads(WAVE_SIZE, 1, 1)]
void ClearTileScreenProbeCacheIndexListLengths (uint DispatchID : SV_DispatchThreadID) {
    if(DispatchID >= UB.TileCount) return ;
    RWTileScreenProbeCacheIndexListLengthsBuffer[DispatchID] = 0;
}

[numthreads(WAVE_SIZE, 1, 1)]
void InitializeScreenProbeCache (uint DispatchID : SV_DispatchThreadID) {
    uint QueueIndex = DispatchID;
    if(QueueIndex >= UB.TileCount) return ;
    // Initialize MRU queue
    RWScreenProbeCacheMRUQueueBuffer[QueueIndex] = QueueIndex;
    // Empty cache
    CacheEntryData NoData = (CacheEntryData)0;
    NoData.bAlive = false;
    RWScreenProbeCacheDataBuffer[QueueIndex] = PackCacheEntry(NoData);
    // Reset flags to no updates
    RWScreenProbeCacheMRUFlagBuffer[QueueIndex] = 0;
    uint CacheEntryIndex = QueueIndex; // For clearing, CacheEntryIndex == QueueIndex
}

groupshared uint SharedScreenProbeSourceHeader;
groupshared uint SharedReprojectedRadiance[TILE_SIZE * TILE_SIZE * 4];
groupshared uint SharedReprojectedSampleCounts[TILE_SIZE * TILE_SIZE];

// Reproject on-screen probes in the previous frame to the current frame
[numthreads(WAVE_SIZE, 1, 1)]
void ReprojectScreenProbes (uint2 GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint2 TileIndex = GroupID.xy;
    CameraParameters C = GetActiveCamera();
    CameraParameters PrevC = GetPreviousCamera();

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

        float SearchSize = LinearDepth * UB.ProbeReprojectionSearchSize 
            * max(C.FilmPixelWorldSize.x, C.FilmPixelWorldSize.y);
        
        uint PreviousProbeHeaderPacked = 0xFFFFFFFFu;
        if(bValidPixel) {
            float3 PreviousNDC = ReprojectToPreviousNDCFromNDC(C, NDC);
            float2 PreviousUV = NDC2ToUV(PreviousNDC.xy);
            if(all(PreviousUV >= 0) && all(PreviousUV < 1)) {
                uint2 PreviousTile = floor(PreviousUV * UB.TileDimensions);
                PreviousProbeHeaderPacked = PreviousTileScreenProbeHeaderTexture.Load(int3(PreviousTile, 0));
                ScreenProbeHeader PreviousProbe = UnpackProbeHeader(PreviousProbeHeaderPacked);
                // Search the previous tiles for a valid probe with the best score.
                if(PreviousProbe.bValid) {
                    float2 PreviousPixelCoords = PreviousProbe.PixelCoords;
                    float2 PreviousUV = ScreenCoordsToUV(PrevC, PreviousPixelCoords);
                    float PreviousReversedZDepth = PreviousDepthTexture.SampleLevel(PointEdgeSampler, PreviousUV, 0);
                    float3 PreviousProbeWorldPosition = RecoverWorldPositionNDC2(
                        PrevC, UVToNDC2(PreviousUV), 
                        ReversedZDepthToLinearDepth(PrevC, PreviousReversedZDepth)
                    );
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

        for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
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
            RWTileScreenProbeHeaderTexture[TileIndex] = INVALID_UINT;
            // Add the tile to the list of projection fail list (prioritized for spawning new probes)
            uint ReprojectionFailListIndex = 0;
            InterlockedAdd(RWReprojectionFailTileCount[0], 1, ReprojectionFailListIndex);
            RWReprojectionFailTileListBuffer[ReprojectionFailListIndex] = PackUint2x16(TileIndex);
        }

        return; // reprojection failed :'(
    }

    // Inject the probe index
    if(WaveIsFirstLane()) {
        ScreenProbeHeader SelectedProbeHeader = (ScreenProbeHeader)0;
        SelectedProbeHeader.PixelCoords = SelectedPixelIndex;
        SelectedProbeHeader.bValid = true;
        RWTileScreenProbeHeaderTexture[TileIndex] = PackProbeHeader(SelectedProbeHeader);
    }


    // And reproject the radiance
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_SIZE * TILE_SIZE; BaseTexelIndex += WAVE_SIZE) {
        uint TileTexelIndex1 = BaseTexelIndex + LocalID;
        float4 RadianceDepth = RecoverRadiance(uint4(SharedReprojectedRadiance[TileTexelIndex1 * 4 + 0],
                                                    SharedReprojectedRadiance[TileTexelIndex1 * 4 + 1],
                                                    SharedReprojectedRadiance[TileTexelIndex1 * 4 + 2],
                                                    SharedReprojectedRadiance[TileTexelIndex1 * 4 + 3]));

        uint SampleCount = SharedReprojectedSampleCounts[TileTexelIndex1];

        if (SampleCount > 0) RadianceDepth /= SampleCount;
        else RadianceDepth = BackupRadianceDepth;
        uint2 TileTexelIndex = TileIndex * TILE_SIZE + uint2(TileTexelIndex1 % TILE_SIZE, TileTexelIndex1 / TILE_SIZE);
        RWScreenProbeRadianceDepthTexture[TileTexelIndex] = RadianceDepth;
    }
}

// Reproject the cached probes and allocate storages (probes that are not not on screen but catched by tile LRU buffers in the previous frame)
[numthreads(WAVE_SIZE, 1, 1)]
void ReprojectCachedProbes (uint DispatchID : SV_DispatchThreadID) {
    uint QueueIndex = DispatchID;
    if(QueueIndex >= UB.TileCount) return;
    uint CacheEntryIndex = RWScreenProbeCacheMRUQueueBuffer[QueueIndex];
    CacheEntryData CacheEntry = UnpackCacheEntry(RWScreenProbeCacheDataBuffer[CacheEntryIndex]);
    float3 ProbeWorldPosition = CacheEntry.WorldPosition;

    CameraParameters C = GetActiveCamera();

    if (CacheEntry.bAlive)
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
            InterlockedAdd(RWScreenProbeCacheIndexReprojectionCount[0], 1, ReprojectionEntryIndex);

            RWScreenProbeCacheIndexReprojectionEntryBuffer[ReprojectionEntryIndex] = uint4(TileEntryIndex, PackUint2x16(ReprojectedScreenCoords), CacheEntryIndex, 0);
        }
    }

    // As well as build RWScreenProbeCacheToMRUQueueIndexBuffer (even if the cache entry is not alive)
    // This will be useful when allocating dead cache entries to new probes
    RWScreenProbeCacheToMRUQueueIndexBuffer[CacheEntryIndex] = QueueIndex;
}

[numthreads(WAVE_SIZE, 1, 1)]
void AllocateTileScreenProbeMRULists (uint DispatchID : SV_DispatchThreadID) {
    uint TileIndex1 = DispatchID;
    if(TileIndex1 >= UB.TileCount) return;

    uint TileEntryBase;
    InterlockedAdd(RWTileScreenProbeCacheIndexListAllocator[0], RWTileScreenProbeCacheIndexListLengthsBuffer[TileIndex1], TileEntryBase);
    RWTileScreenProbeCacheIndexListOffsetsBuffer[TileIndex1] = TileEntryBase;
}

// Scatter the reprojected probes to finish the reprojected cached probe list index for each tile
[numthreads(WAVE_SIZE, 1, 1)]
void ScatterReprojectedCachedProbesToMRUList (uint DispatchID : SV_DispatchThreadID) {
    uint ReprojectionEntryIndex = DispatchID;
    if(ReprojectionEntryIndex >= RWScreenProbeCacheIndexReprojectionCount[0]) return;
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
            SpawnListWaveRank = WavePrefixCountBits(true);
            uint SpawnListWaveSum = WaveActiveCountBits(true);
            uint SpawnListWaveIndexBase = 0;
            if(WaveIsFirstLane()) {
                InterlockedAdd(
                    RWScreenProbeSpawnCount[0],
                    SpawnListWaveSum, SpawnListWaveIndexBase
                );
            }
            SpawnListWaveIndexBase = WaveReadLaneFirst(SpawnListWaveIndexBase);
            uint SpawnListIndex = SpawnListWaveIndexBase + SpawnListWaveRank;
            if(SpawnListIndex < UB.MaxProbesToSpawnPerFrame) {
                // Write the header to the spawn list
                ScreenProbeHeader Header = (ScreenProbeHeader)0;
                Header.bValid = true;
                Header.PixelCoords = ScreenCoords;
                RWScreenProbeSpawnListBuffer[SpawnListIndex] = PackProbeHeader(Header);
            }
        }
    }
}

// Prioritize the spawnning of probes in reprojection fail tiles by substituting them into the spawn list
// Patch holes for reprojection dissoclusions 
[numthreads(WAVE_SIZE, 1, 1)]
void SubstituteScreenProbes (uint DispatchID : SV_DispatchThreadID) {
    uint FailListIndex = DispatchID;
    uint FailTileCount = RWReprojectionFailTileCount[0];
    if(FailListIndex >= FailTileCount) return;
    uint2 FailTileIndex = UnpackUint2x16(RWReprojectionFailTileListBuffer[FailListIndex]);
    uint SpawnProbeCount = min(RWScreenProbeSpawnCount[0], UB.MaxProbesToSpawnPerFrame);
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


[numthreads(1, 1, 1)]
void UpdateScreenProbeSpawnCount () {
    RWScreenProbeSpawnCount[0] = 
        min(
            RWScreenProbeSpawnCount[0] + RWReprojectionFailTileCount[0],
            UB.MaxProbesToSpawnPerFrame
        );
}

float RadianceToSampleWeight (float3 Radiance) {
    return RadianceToLuminance(Radiance) + 1e-5f;
}

#define MAX_NUM_UPDATE_RAYS_PER_PROBE  (2 * TILE_TEXEL_COUNT)

groupshared uint4 SharedProbeBlendedRadiance[TILE_SIZE * TILE_SIZE];
groupshared float SharedProbeOctahedronSampleWeight[TILE_SIZE * TILE_SIZE];
groupshared float SharedProbeOctahedronSampleWeightPrefixSum[TILE_SIZE * TILE_SIZE];
// For each probe to be spawned, reconstruct radiance & sample update rays and locate probe cache entries to update/evict
[numthreads(WAVE_SIZE, 1, 1)]
void ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries (uint GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint SpawnListIndex = GroupID;
    uint SpawnListCount = min(
        RWScreenProbeSpawnCount[0],
        UB.MaxProbesToSpawnPerFrame
    );
    if(SpawnListIndex >= SpawnListCount) return;

    ScreenProbeHeader Header = UnpackProbeHeader(RWScreenProbeSpawnListBuffer[SpawnListIndex]);
    uint2 TileIndex = Header.PixelCoords / TILE_SIZE;
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
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

    // Properties of the reprojected probe from last frame (if any) in the same tile
    // (the reprojected probe is going to be evicted by the newly spawned probe)
    uint PackedReprojectedProbeHeader = RWTileScreenProbeHeaderTexture[TileIndex];
    ScreenProbeHeader ReprojectedProbe = UnpackProbeHeader(PackedReprojectedProbeHeader);
    float3 ReprojectedProbeWorldPos = 0;
    float3 ReprojectedProbeNormal = 0;
    if(ReprojectedProbe.bValid) {
        float2 ReprojectedProbeUV = (ReprojectedProbe.PixelCoords + 0.5f) * C.InvFilmDimensions;
        float ReprojectedProbeLinearDepth = ReversedZDepthToLinearDepth(C, G_Depth.SampleLevel(PointEdgeSampler, ReprojectedProbeUV, 0).x);
        ReprojectedProbeWorldPos = RecoverWorldPositionNDC2(C, UVToNDC2(ReprojectedProbeUV), ReprojectedProbeLinearDepth);
        ReprojectedProbeNormal = normalize(G_Normal.SampleLevel(PointEdgeSampler, ReprojectedProbeUV, 0).rgb * 2 - 1);
    }


    float SearchSize = LinearDepth * UB.ProbeReprojectionSearchSize
            * max(C.FilmPixelWorldSize.x, C.FilmPixelWorldSize.y);
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
            for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
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
    float MinReprojectedProbeScoreFromCache = 1e+10f;
    uint  MinReprojectedProbeScoreCacheEntryIndex = INVALID_UINT;
    for(int dx = -1; dx <= 1; dx++) {
        for(int dy = -1; dy <= 1; dy++) {
            int2 NeighborTileIndex = TileIndex + int2(dx, dy);
            if(any(NeighborTileIndex < 0) || any(NeighborTileIndex >= UB.TileDimensions)) continue ;
            uint NeighborTileIndex1 = NeighborTileIndex.x + NeighborTileIndex.y * UB.TileDimensions.x;
            uint TileCacheIndexListLenght = RWTileScreenProbeCacheIndexListLengthsBuffer[NeighborTileIndex1];
            uint TileCacheIndexListOffset = RWTileScreenProbeCacheIndexListOffsetsBuffer[NeighborTileIndex1];
            for(uint ProbeMRUListRank = 0; ProbeMRUListRank < TileCacheIndexListLenght; ProbeMRUListRank++) {
                uint TileCacheIndexListIndex = TileCacheIndexListOffset + ProbeMRUListRank;
                uint CacheEntryIndex = RWTileScreenProbeCacheIndexListBuffer[TileCacheIndexListIndex];
                uint4 PackedCacheEntry = RWScreenProbeCacheDataBuffer[CacheEntryIndex];
                CacheEntryData CacheEntry = UnpackCacheEntry(PackedCacheEntry);
                float3 CachedProbeWorldPos = CacheEntry.WorldPosition;
                float3 CachedProbeNormal = CacheEntry.Normal;
                if(abs(dot(CachedProbeWorldPos - WorldPosition, Normal)) < SearchSize) {
                    float3 CachedProbeTangent, CachedProbeBitangent;
                    GetOrthoVectors(CachedProbeNormal, CachedProbeTangent, CachedProbeBitangent);
                    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
                        uint TexelIndex = BaseTexelIndex + LocalID;
                        uint2 ProbeTexelCoords = int2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
                        uint2 AtlasTexelCoords = NeighborTileIndex * TILE_SIZE + ProbeTexelCoords;
                        float4 ProbeRadianceDepth = RWScreenProbeCacheRadianceDepthTexture[AtlasTexelCoords];
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
                if(abs(dot(CachedProbeWorldPos - ReprojectedProbeWorldPos, Normal)) < SearchSize && ReprojectedProbe.bValid) {
                    float ProbeScore = distance(CachedProbeWorldPos, ReprojectedProbeWorldPos);
                    if(dot(ReprojectedProbeNormal, CachedProbeNormal) >= 0.95f && ProbeScore < MinReprojectedProbeScoreFromCache) {
                        MinReprojectedProbeScoreFromCache = ProbeScore;
                        MinReprojectedProbeScoreCacheEntryIndex = CacheEntryIndex;
                    }
                }
            }
        }
    }
    ReusedProbeTexelCount = WaveActiveSum(ReusedProbeTexelCount);

    // Write out the cache to substitute & update if the reprojected probe is present
    if(WaveIsFirstLane()) {
        if(ReprojectedProbe.bValid) {
            if(IsValid(MinReprojectedProbeScoreCacheEntryIndex)) {
                // Make sure that we're not starting multiple writes to one cache entry
                uint Original;
                InterlockedCompareExchange(RWScreenProbeCacheMRUFlagBuffer[MinReprojectedProbeScoreCacheEntryIndex], 0, 1, Original);
                if(Original != 0) {
                    // Someone else has taken this cache entry, so we just drop the cache write
                    MinReprojectedProbeScoreCacheEntryIndex = INVALID_UINT;
                }
            }
        }
        if(IsValid(MinUpdateProbeScoreCacheEntryIndex)) {
            // Make sure that we're not starting multiple writes to one cache entry
            uint Original;
            InterlockedCompareExchange(RWScreenProbeCacheMRUFlagBuffer[MinUpdateProbeScoreCacheEntryIndex], 0, 1, Original);
            if(Original != 0) {
                // Someone else has taken this cache entry, so we just drop the cache update
                MinUpdateProbeScoreCacheEntryIndex = INVALID_UINT;
            }
        }
        RWScreenProbeSpawnCacheMatchesBuffer[SpawnListIndex] = uint2(MinReprojectedProbeScoreCacheEntryIndex, MinUpdateProbeScoreCacheEntryIndex);
    }
    // Sample rays
    // We assume that ray count is always a multiple of WAVE_SIZE
    // Prepare probe local coordinate system
    float3 ProbeTangent, ProbeBitangent;
    GetOrthoVectors(Normal, ProbeTangent, ProbeBitangent);

    const float Epsilon = 1e-5f;

    float SumSizeOctahedronOriginal = 0.f;
    // Resolve octahedron sample weights
    {
        float SavedPrefixSum = 0.f;
        for(int BaseProbeTexelIndex = 0; BaseProbeTexelIndex < TILE_TEXEL_COUNT; BaseProbeTexelIndex += WAVE_SIZE) {
            int  ProbeTexelIndex = BaseProbeTexelIndex + LocalID;
            int2 ProbeTexelCoords = int2(ProbeTexelIndex % TILE_SIZE, ProbeTexelIndex / TILE_SIZE);
            float4 RadianceDepth = RecoverRadiance(uint4(SharedProbeBlendedRadiance[ProbeTexelIndex].x,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].y,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].z,
                                                  SharedProbeBlendedRadiance[ProbeTexelIndex].w));
            // Write out reconstructed radiance to a separate buffer for later blending
            RWScreenProbeReconstructedRadianceDepthBuffer[TileIndex * TILE_SIZE + ProbeTexelCoords] = RadianceDepth;
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
    NumProbeOctahedronSamples = max(NumProbeOctahedronSamples, MAX_NUM_UPDATE_RAYS_PER_PROBE);
    uint UpdateRayIndexBase = 0;
    if(WaveIsFirstLane()) {
        InterlockedAdd(RWScreenProbeUpdateRayAllocator[0], NumProbeOctahedronSamples, UpdateRayIndexBase);
        uint RemainingRayCount = (UB.MaxNumUpdateRays <= UpdateRayIndexBase) ? 0 : UB.MaxNumUpdateRays - UpdateRayIndexBase;
        NumProbeOctahedronSamples = min(NumProbeOctahedronSamples, RemainingRayCount);
        RWScreenProbeUpdateRayCountsBuffer[SpawnListIndex] = NumProbeOctahedronSamples;
        RWScreenProbeUpdateRayOffsetsBuffer[SpawnListIndex] = UpdateRayIndexBase;
    }
    NumProbeOctahedronSamples = WaveReadLaneFirst(NumProbeOctahedronSamples);
    UpdateRayIndexBase = WaveReadLaneFirst(UpdateRayIndexBase);

    // If the probe has adequate samples from radiance reconstruction, mark it as temporal blendable
    if(WaveIsFirstLane()) {
        if(ReusedProbeTexelCount >= 32) {
            RWScreenProbeSpawnListBuffer[SpawnListIndex] = 
                ProbeHeaderMarkTemporalBlendable(RWScreenProbeSpawnListBuffer[SpawnListIndex]); // Mark as temporal blendable
        }
    }

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
#define MIN_PDF_TO_TRACE 1e-2f
		if(RayPdf >= MIN_PDF_TO_TRACE && bValid) {
			// A valid update ray is spawned
			// Queue up for a ray trace
            RWScreenProbeUpdateRayDirectionBuffer[RayIndex] = PackNormal(RayDirection);
            RWScreenProbeUpdateRayStateBuffer[RayIndex] = 0; // Initial state
            RWScreenProbeUpdateRayOriginScreenCoordsBuffer[RayIndex] = PackUint2x16(Header.PixelCoords);
			RWScreenProbeUpdateRayResultBuffer[RayIndex] = PackFp16x4Safe(0.f.xxxx);
			// Keep extra data for later probe update
			float RayInvPdf = 1.f / RayPdf;
            RWScreenProbeUpdateRayInvPdfBuffer[RayIndex] = RayInvPdf;
        } else {
            // Invalid update ray, mark it as invalid to skip tracing
            RWScreenProbeUpdateRayDirectionBuffer[RayIndex] = PackNormal(0.f.xxx);
            RWScreenProbeUpdateRayStateBuffer[RayIndex] = 0; // Initial state
            RWScreenProbeUpdateRayOriginScreenCoordsBuffer[RayIndex] = 0;
            RWScreenProbeUpdateRayResultBuffer[RayIndex] = PackFp16x4Safe(0.f.xxxx);
            RWScreenProbeUpdateRayInvPdfBuffer[RayIndex] = 0.f;
        }
    }
}

[numthreads(WAVE_SIZE, 1, 1)]
void ClipUpdateRayCount () {
    RWScreenProbeUpdateRayAllocator[0] = min(RWScreenProbeUpdateRayAllocator[0], UB.MaxNumUpdateRays);
}

// The sampled rays are traced in separate shaders via hybrid tracing
// Radiance results are stored in RWScreenProbeUpdateRayResultBuffer

// Update screen probes & cache
groupshared uint SharedProbeSampleCounts[TILE_SIZE * TILE_SIZE];
[numthreads(WAVE_SIZE, 1, 1)]
void UpdateScreenProbesAndCache (uint GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint SpawnListIndex = GroupID;
    uint SpawnListCount = RWScreenProbeSpawnCount[0];
    if(SpawnListIndex >= SpawnListCount) return;

    // Clear the shared memory for later ray radiance accumulation
    ScreenProbeHeader Header = UnpackProbeHeader(RWScreenProbeSpawnListBuffer[SpawnListIndex]);
    uint2 TileIndex = Header.PixelCoords / TILE_SIZE;
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        SharedProbeBlendedRadiance[TexelIndex] = 0;
        SharedProbeSampleCounts[TexelIndex] = 0;
    }
    uint PackedReprojectedProbeHeader = RWTileScreenProbeHeaderTexture[TileIndex];
    ScreenProbeHeader ReprojectedProbe = UnpackProbeHeader(PackedReprojectedProbeHeader);
    bool bReprojectedProbeValid = ReprojectedProbe.bValid;
    uint2 ReprojectedProbeIndex = ReprojectedProbe.PixelCoords / TILE_SIZE;
    CameraParameters C = GetActiveCamera();
    float2 ReprojectedProbeUV = (ReprojectedProbe.PixelCoords + 0.5f) * C.InvFilmDimensions;

    GroupMemoryBarrierWithGroupSync();

    float SumRayWeight = 0;
    float4 SumRayResult = 0;

    // Accumulate radiance from traced update rays
    uint ProbeUpdateRayBase = RWScreenProbeUpdateRayOffsetsBuffer[SpawnListIndex];
    uint UpdateRayCount = RWScreenProbeUpdateRayCountsBuffer[SpawnListIndex];
    // Assume UpdateRayCount is a multiple of WAVE_SIZE, which is guaranteed by the previous shaders
    for(uint BaseRayRank = 0; BaseRayRank < UpdateRayCount; BaseRayRank += WAVE_SIZE) {
        uint RayRank = BaseRayRank + LocalID;
        uint RayIndex = ProbeUpdateRayBase + RayRank;
        float4 RayResult = UnpackFp16x4Safe(RWScreenProbeUpdateRayResultBuffer[RayIndex]);
        // The hit distance is stored in RWScreenProbeUpdateRayStateBuffer[RayIndex]
        bool bHit;
        RayResult.w = UnpackRayToTraceState(RWScreenProbeUpdateRayStateBuffer[RayIndex], bHit);
        float RayInvPdf = RWScreenProbeUpdateRayInvPdfBuffer[RayIndex];
        bool bValid = RayInvPdf > 0 && RayResult.w > 0;
        if(bValid) {
            float3 RayDirection = UnpackNormal(RWScreenProbeUpdateRayDirectionBuffer[RayIndex]);
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

    if(bReprojectedProbeValid && IsInvalid(ProbeToEvictCacheEntryIndex)) {
        float ReprojectedProbeReversedZDepth = G_Depth.SampleLevel(PointEdgeSampler, ReprojectedProbeUV, 0).x;
        float ReprojectedProbeLinearDepth = ReversedZDepthToLinearDepth(C, ReprojectedProbeReversedZDepth);
        float3 ReprojectedProbeWorldPos = RecoverWorldPositionPixelCoords(C, ReprojectedProbe.PixelCoords, ReprojectedProbeLinearDepth);
        float3 ReprojectedProbeNormal = normalize(G_Normal.SampleLevel(PointEdgeSampler, ReprojectedProbeUV, 0).rgb * 2 - 1);

        // The reprojected probe have to be allocated a new cache entry
        // Try to allocate a new cache entry
        uint CacheEntryIndex = INVALID_UINT;
        if(WaveIsFirstLane()) {
            uint QueueIndex = 0;
            InterlockedAdd(RWScreenProbeCacheMRUQueueEntryAllocator[0], 1, QueueIndex);
            if(QueueIndex < UB.TileCount) {
                // Use the oldest elements in the MRU queue first.
                CacheEntryIndex = RWScreenProbeCacheMRUQueueBuffer[UB.TileCount - QueueIndex - 1];
                // Write metadata
                CacheEntryData NewCacheEntry = (CacheEntryData)0;
                NewCacheEntry.bAlive = true;
                NewCacheEntry.WorldPosition = ReprojectedProbeWorldPos;
                NewCacheEntry.Normal = ReprojectedProbeNormal;
                RWScreenProbeCacheDataBuffer[CacheEntryIndex] 
                    = PackCacheEntry(NewCacheEntry);
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
        float4 NewRadiance = RecoverRadiance(NewRadianceQuantized) / float(max(SampleCount, 1));
        if(SampleCount == 0) {
            NewRadiance = BackupRayResult;
        }
        // Temporal blending with the reconstructed radiance from previous frames.
        if (Header.bTemporalBlendable)
        {
            float4 ReconstructedRadiance = RWScreenProbeReconstructedRadianceDepthBuffer[AtlasTexelCoords];
            float lumaA = RadianceToLuminance(NewRadiance.xyz);
            float lumaB = RadianceToLuminance(ReconstructedRadiance.xyz);

            // Shadow-preserving biased temporal hysteresis (inspired by: https://www.youtube.com/watch?v=WzpLWzGvFK4&t=630s)
            float temporal_blend = Squared(clamp(max(lumaA - lumaB - min(lumaA, lumaB), 0.0f) / max(max(lumaA, lumaB), 1e-4f), 0.0f, 0.95f));

            NewRadiance = lerp(NewRadiance, ReconstructedRadiance, temporal_blend);
        }

        // Write radiance values of the reprojected probe from previous frame to the evicted cache entry (if any)
        if(bReprojectedProbeValid) {
            if(ProbeToEvictCacheEntryIndex != INVALID_UINT) {
                // Have to evict a cache entry (replaced by the reprojected probe)
                uint2 CacheAtlasIndex = uint2(
                    ProbeToEvictCacheEntryIndex % UB.TileDimensions.x,
                    ProbeToEvictCacheEntryIndex / UB.TileDimensions.x
                );
                uint2 CacheAtlasBaseCoords = CacheAtlasIndex * TILE_SIZE;
                uint2 ProbeAtlasBaseCoords = ReprojectedProbeIndex * TILE_SIZE;
                // Copy the reprojected probe to the cache entry
                uint2 CacheAtlasTexelCoords = CacheAtlasBaseCoords + TexelCoords;
                uint2 ProbeAtlasTexelCoords = ProbeAtlasBaseCoords + TexelCoords;
                RWScreenProbeCacheRadianceDepthTexture[CacheAtlasTexelCoords] = 
                    RWScreenProbeRadianceDepthTexture[ProbeAtlasTexelCoords];
            } else {
                // This should never happen if cache has not been overflown
            }
        }

        // Update the closest cache entry to the current probe (if any)
        if(ProbeToUpdateCacheEntryIndex != INVALID_UINT) {
            // Update an existing cache entry
            uint2 CacheAtlasIndex = uint2(
                ProbeToUpdateCacheEntryIndex % UB.TileDimensions.x,
                ProbeToUpdateCacheEntryIndex / UB.TileDimensions.x
            );
            uint2 CacheAtlasBaseCoords = CacheAtlasIndex * TILE_SIZE;
            uint2 CacheAtlasTexelCoords = CacheAtlasBaseCoords + TexelCoords;
            RWScreenProbeCacheRadianceDepthTexture[CacheAtlasTexelCoords] = NewRadiance;
        }

        // Update foreground radiance weight atlas
        RWScreenProbeRadianceDepthTexture[AtlasTexelCoords] = NewRadiance;
    }

    if(WaveIsFirstLane()) {
        // Finally, update the header for indexing the newly spawned probe
        RWTileScreenProbeHeaderTexture[TileIndex] = PackProbeHeader(Header);
    }
}

// A scan-sum is performed on RWScreenProbeCacheMRUFlagBuffer
// Results are accumulated into RWScreenProbeCacheMRUFlagPrefixSumBuffer

// Update MRU queue, putting recently used entries to the front
[numthreads(WAVE_SIZE, 1, 1)]
void UpdateScreenProbeCacheMRUQueue (uint DispatchID : SV_DispatchThreadID) {
    uint QueueIndex = DispatchID;
    if(QueueIndex >= UB.TileCount) return;
    uint Element = RWScreenProbeCacheMRUQueueBuffer[QueueIndex];
    uint Flag = RWScreenProbeCacheMRUFlagBuffer[QueueIndex];
    uint PrefixSum = RWScreenProbeCacheMRUFlagPrefixSumBuffer[QueueIndex];
    if(Flag) {
        RWScreenProbeCacheUpdatedMRUQueueBuffer[PrefixSum - 1] = Element;
        // Reset the flag for next frame
        RWScreenProbeCacheMRUFlagBuffer[QueueIndex] = 0;
    } else {
        uint AllSum = RWScreenProbeCacheMRUFlagPrefixSumBuffer[UB.TileCount - 1];
        uint Rank = QueueIndex - PrefixSum;
        RWScreenProbeCacheUpdatedMRUQueueBuffer[AllSum + Rank] = Element;
    }
}

RWTexture2D<uint> RWInTileScreenProbeHeaderTexture;
RWTexture2D<uint> RWOutTileScreenProbeHeaderTexture;

// Update the mips of RWTileScreenProbeHeaderTexture
[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void MakeTileScreenProbeHeaderIndex(uint2 DispatchID : SV_DispatchThreadID)
{
    uint2 Dimensions;
    RWInTileScreenProbeHeaderTexture.GetDimensions(Dimensions.x, Dimensions.y);

    if (any((DispatchID << 1) >= Dimensions))
    {
        return; // out of bounds
    }

    uint PackedHeader = INVALID_UINT;

    // Here, we look for a valid probe in the 2x2 region from the upper mip in
    // order to populate the lower mips with as many valid probes as possible.
    for (uint y = 0; y < 2; ++y)
    {
        for (uint x = 0; x < 2; ++x)
        {
            uint2 PixelCoords = (DispatchID << 1) + uint2(x, y);
            PackedHeader = (all(PixelCoords < Dimensions) ? RWInTileScreenProbeHeaderTexture[PixelCoords] : INVALID_UINT);
            if (IsValid(PackedHeader)) break;
        }

        if (IsValid(PackedHeader)) break;
    }

    RWOutTileScreenProbeHeaderTexture[DispatchID] = PackedHeader;
}

void WriteScreenProbeSHCoefficients (uint2 ProbeIndex, float3 SHCoefficients[9]) {
    float4 R1 = float4(SHCoefficients[1].x, SHCoefficients[2].x, SHCoefficients[3].x, SHCoefficients[4].x);
    float4 R2 = float4(SHCoefficients[5].x, SHCoefficients[6].x, SHCoefficients[7].x, SHCoefficients[8].x);
    float4 G1 = float4(SHCoefficients[1].y, SHCoefficients[2].y, SHCoefficients[3].y, SHCoefficients[4].y);
    float4 G2 = float4(SHCoefficients[5].y, SHCoefficients[6].y, SHCoefficients[7].y, SHCoefficients[8].y);
    float4 B1 = float4(SHCoefficients[1].z, SHCoefficients[2].z, SHCoefficients[3].z, SHCoefficients[4].z);
    float4 B2 = float4(SHCoefficients[5].z, SHCoefficients[6].z, SHCoefficients[7].z, SHCoefficients[8].z);
    float3 RGB0 = SHCoefficients[0];
    RWScreenProbeIrradianceTexture[ProbeIndex].xyz = RGB0;
    RWScreenProbeSHCoefficientsRTexture[ProbeIndex] = R1;
    RWScreenProbeSHCoefficientsRTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)] = R2;
    RWScreenProbeSHCoefficientsBTexture[ProbeIndex] = B1;
    RWScreenProbeSHCoefficientsBTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)] = B2;
    RWScreenProbeSHCoefficientsGTexture[ProbeIndex] = G1;
    RWScreenProbeSHCoefficientsGTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)] = G2;
}

void GetScreenProbeSHCoefficients (int2 ProbeIndex, out float3 ProbeSH[9]) {
    float4 R1 = RWScreenProbeSHCoefficientsRTexture[ProbeIndex];
    float4 R2 = RWScreenProbeSHCoefficientsRTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)];
    float4 G1 = RWScreenProbeSHCoefficientsGTexture[ProbeIndex];
    float4 G2 = RWScreenProbeSHCoefficientsGTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)];
    float4 B1 = RWScreenProbeSHCoefficientsBTexture[ProbeIndex];
    float4 B2 = RWScreenProbeSHCoefficientsBTexture[ProbeIndex + int2(UB.TileDimensions.x, 0)];
    float3 RGB0 = RWScreenProbeIrradianceTexture[ProbeIndex].xyz;
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
void ComputeScreenProbeSHCoefficients (uint2 GroupID : SV_GroupID, uint LocalID : SV_GroupThreadID) {
    uint2 TileIndex = GroupID;
    ScreenProbeHeader Header = UnpackProbeHeader(RWTileScreenProbeHeaderTexture[TileIndex]);
    if(!Header.bValid) return;
    CameraParameters C = GetActiveCamera();
    float2 ProbeUV = (Header.PixelCoords + 0.5f) * C.InvFilmDimensions;
    float3 ProbeNormal = normalize(G_Normal.SampleLevel(PointEdgeSampler, ProbeUV, 0).rgb * 2 - 1);
    float3 ProbeTangent, ProbeBitangent;
    GetOrthoVectors(ProbeNormal, ProbeTangent, ProbeBitangent);

    uint2 ProbeAtlasBaseCoords = TileIndex * TILE_SIZE;

    // Clear SH coefficients
    float3 SHCoefficients[9];
    [unroll] for(uint i = 0; i < 9; i++) {
        SHCoefficients[i] = 0;
    }

    // Accumulate SH coefficients
    for(uint BaseTexelIndex = 0; BaseTexelIndex < TILE_TEXEL_COUNT; BaseTexelIndex += WAVE_SIZE) {
        uint TexelIndex = BaseTexelIndex + LocalID;
        uint2 TexelCoords = uint2(TexelIndex % TILE_SIZE, TexelIndex / TILE_SIZE);
        uint2 AtlasTexelCoords = ProbeAtlasBaseCoords + TexelCoords;
        float4 RadianceDepth = RWScreenProbeRadianceDepthTexture[AtlasTexelCoords];
        float3 Radiance = RadianceDepth.xyz;
        float2 TexelUV = (float2(TexelCoords) + 0.5f) / TILE_SIZE;
        float3 LocalTexelDirection = HemiOctahedron01ToUnitVectorA(TexelUV);
        float3 ProbeWorldTexelDirection = 
            LocalTexelDirection.x * ProbeTangent 
            + LocalTexelDirection.y * ProbeBitangent
            + LocalTexelDirection.z * ProbeNormal;
        
        float AreaCorrectionFactor = 1.f;
        // float Weight = AreaCorrectionFactor * (TWO_PI / TILE_TEXEL_COUNT);
        // Accumulate SH coefficients
        float  Coefficients[9];
        SH_GetCoefficients(ProbeWorldTexelDirection, Coefficients);
        float AreaCorrection = 1.f;
        for(int i = 0; i < 9; i++) {
            SHCoefficients[i] += Coefficients[i] * Radiance * AreaCorrection;
        }
    }
    // Write SH coefficients
    for(uint i = 0; i<9; i++) {
        // Multiply by TWO_PI to monte-carlo integrate to retrieve the coefficients
        // (TWO_PI: Hemispherical integration)
        SHCoefficients[i] = WaveActiveSum(SHCoefficients[i]) * (1.f / TILE_TEXEL_COUNT) * TWO_PI;
    }
    if(WaveIsFirstLane()) {
        WriteScreenProbeSHCoefficients(TileIndex, SHCoefficients);
    }
}

// Modified from GI1.0
// Evaluates the irradiance from the probe's SH representation using a bent cone.
float3 ProbeIntegrateBentCone(float3 Normal, float AO, int2 TileIndex)
{
    float ClampedCosineSH[9];
    SH_GetCoefficients_ClampedCosine_Cone(Normal, acos(sqrt(saturate(1.0f - AO))), ClampedCosineSH);

    float3 Irradiance = float3(0.0f, 0.0f, 0.0f);
    float3 ProbeSH[9];
    GetScreenProbeSHCoefficients(TileIndex, ProbeSH);
    for (uint i = 0; i < 9; ++i)
    {
        Irradiance += ClampedCosineSH[i] * ProbeSH[i];
    }

    return max(Irradiance, 0.0f);
}

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void ComputeDiffuseIndirectLighting(uint2 GroupID : SV_GroupID, uint2 LocalID : SV_GroupThreadID)
{
    uint2 PixelCoords = GroupID * TILE_SIZE + LocalID;
    CameraParameters C = GetActiveCamera();
    if (any(PixelCoords >= C.FilmDimensions)) return;
    float  ReversedZDepth = G_Depth.Load(uint3(PixelCoords, 0)).x;
    float3 Normal         = normalize(G_Normal.Load(uint3(PixelCoords, 0)).xyz * 2 - 1);
    if (ReversedZDepth == 0)
    {
        RWDiffuseIndirectLightingTexture[PixelCoords] = 0;
        return; 
    }
    float2 UV = (PixelCoords + 0.5f) * C.InvFilmDimensions;
    float LinearDepth = ReversedZDepthToLinearDepth(C, ReversedZDepth);
    float3 WorldPosition = RecoverWorldPositionNDC2(C, UVToNDC2(UV), LinearDepth);
    float  SearchSize = LinearDepth * UB.ProbeReprojectionSearchSize
            * max(C.FilmPixelWorldSize.x, C.FilmPixelWorldSize.y);

    uint4 NearbyProbes;   // locate nearby probes for interpolation

    NearbyProbes.x = FindClosestScreenProbe(PixelCoords);

    if (IsInvalid(NearbyProbes.x))
    {
        RWDiffuseIndirectLightingTexture[PixelCoords] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return; // couldn't find any nearby probe...
    }

    ScreenProbeHeader MajorHeader = UnpackProbeHeader(NearbyProbes.x);
    int2 PixelProbeOffset = int2(PixelCoords.x < MajorHeader.PixelCoords.x ? -1 : 1, PixelCoords.y < MajorHeader.PixelCoords.y ? -1 : 1);

    NearbyProbes.y = FindClosestScreenProbe(PixelCoords, int2(PixelProbeOffset.x, 0));
    NearbyProbes.z = FindClosestScreenProbe(PixelCoords, int2(0, PixelProbeOffset.y));
    NearbyProbes.w = FindClosestScreenProbe(PixelCoords, PixelProbeOffset);

    if (NearbyProbes.y == NearbyProbes.x) NearbyProbes.y = INVALID_UINT;
    if (NearbyProbes.z == NearbyProbes.y || NearbyProbes.z == NearbyProbes.x) NearbyProbes.z = INVALID_UINT;
    if (NearbyProbes.w == NearbyProbes.z || NearbyProbes.w == NearbyProbes.y 
    || NearbyProbes.w == NearbyProbes.x) NearbyProbes.w = INVALID_UINT;

    float4 NearbyProbeWeights = 0;  // calculate per-probe blending weights

    for (uint i = 0; i < 4; i++)
    {
        if (IsValid(NearbyProbes[i]))
        {
            // uint2 probe_seed = ScreenProbes_UnpackSeed(probes[i]);
            ScreenProbeHeader Header = UnpackProbeHeader(NearbyProbes[i]);

            float2 ProbeUV    = (Header.PixelCoords + 0.5f) * C.InvFilmDimensions;
            float  ProbeReversedZDepth = G_Depth.Load(int3(Header.PixelCoords, 0)).x;
            float  ProbeLinearDepth = ReversedZDepthToLinearDepth(C, ProbeReversedZDepth);
            float3 ProbeWorldPosition  = RecoverWorldPositionNDC2(C, UVToNDC2(ProbeUV), ProbeLinearDepth);
            float3 ProbeNormal = normalize(G_Normal.Load(int3(Header.PixelCoords, 0)).xyz * 2 - 1);

            if (abs(dot(ProbeWorldPosition - WorldPosition, Normal)) > SearchSize)
                NearbyProbeWeights[i] = 0.0f;    // prevent probes ahead of pixel plane to leak radiance into occluded background
            else
            {
                NearbyProbeWeights[i]  = saturate(1.0f - abs(ProbeLinearDepth - LinearDepth) / max(LinearDepth, 1e-5f));
                NearbyProbeWeights[i] *= saturate(dot(Normal, ProbeNormal));
                NearbyProbeWeights[i]  = pow(NearbyProbeWeights[i], 8.0f);    // make it steep
            }
        }
    }

    bool bUseBackup = false;

    if (dot(NearbyProbeWeights, NearbyProbeWeights) == 0.0f)
    {
        NearbyProbeWeights = 
            float4(1.0f, 
                IsValid(NearbyProbes.y) ? 1.0f : 0.0f,
                IsValid(NearbyProbes.z) ? 1.0f : 0.0f,
                IsValid(NearbyProbes.w) ? 1.0f : 0.0f);

        bUseBackup = true;  // for 'relaxed' interpolation in failure cases
    }

    NearbyProbeWeights /= dot(NearbyProbeWeights, 1.f.xxxx);

    float3 Irradiance = 0.f;
    for (uint i = 0; i < 4; i++)
    {
        ScreenProbeHeader Header = UnpackProbeHeader(NearbyProbes[i]);
        if(Header.bValid) {
            uint2 CurrentProbeTileIndex = Header.PixelCoords / TILE_SIZE;
            Irradiance += NearbyProbeWeights[i] * ProbeIntegrateBentCone(Normal, 1.f, CurrentProbeTileIndex);
        }
    }

    RWDiffuseIndirectLightingTexture[PixelCoords] = float4(Irradiance, bUseBackup ? 0 : 1);

}