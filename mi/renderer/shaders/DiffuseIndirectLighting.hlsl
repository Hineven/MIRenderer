#include "headers/Conventions.hlsl"
#include "headers/Camera.hlsl"
#include "headers/GeometryBuffers.hlsl"
#include "headers/Packing.hlsl"
#include "headers/OctahedronMapping.hlsl"
#include "resources/CommonSamplerResources.hlsl"

[[vk::image_format(rgba16f)]]
Texture2D<float4> PreviousScreenProbeRadianceDepthTexture;
[[vk::image_format(rgba16f)]]
RWTexture2D<float4> RWScreenProbeRadianceDepthTexture;

StructuredBuffer<uint> PreviousTileScreenProbeMRUQueueBuffer; // Probe index
StructuredBuffer<uint> PreviousTileScreenProbeMRUQueueLengthBuffer;

RWStructuredBuffer<uint> RWTileScreenProbeMRUQueueBuffer;
RWStructuredBuffer<uint> RWTileScreenProbeMRUQueueLengthBuffer;

// A mipmapped texture (of resolution TileDimensions), including the closest probe header within tiles.
Texture2D<uint> PreviousTileScreenProbeHeaderTexture;
// Mip 0
RWTexture2D<uint> RWTileScreenProbeHeaderTexture;

// The list of reprojection occlusion tiles
RWStructuredBuffer<uint> RWReprojectionFailTileCountBuffer;
RWStructuredBuffer<uint> RWReprojectionFailTileListBuffer; // uint16x2 packed tile index

// The list of new probes to spawn in this frame
RWStructuredBuffer<uint> RWScreenProbeSpawnCountBuffer;
// Packed probe headers
RWStructuredBuffer<uint> RWScreenProbeSpawnListBuffer;


// Position x 12bytes + Unused x 4bytes
StructuredBuffer<uint4> ProbeCacheEntryBuffer;

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
    float Padding;
    float2 InvProbeAtlasDimensions;
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

                    if(abs(dot(PreviousProbeWorldPosition - WorldPosition, Normal)) < SearchSize && dot(PreviousProbeWorldNormal, Normal) > 0.95f) {
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
    
    if (SelectedSrcProbeThread != 0xFFFFu)
    {
        ScreenProbeHeader PrevProbeHeader = UnpackProbeHeader(SharedScreenProbeSourceHeader);
        uint2 SelectedPixelIndex = uint2(SelectedSrcProbeThread % TILE_SIZE, SelectedSrcProbeThread / TILE_SIZE) + TileIndex * TILE_SIZE;
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
        if (LocalID == 0)
        {
            // Mark this probe as invalid
            RWTileScreenProbeHeaderTexture[GroupID] = INVALID_UINT;
            // Add the tile to the list of occlusions
            uint ReprojectionFailListIndex = 0;
            InterlockedAdd(RWReprojectionFailTileCountBuffer[0], 1, ReprojectionFailListIndex);
            RWReprojectionFailTileListBuffer[ReprojectionFailListIndex] = PackUint2x16(GroupID);
        }

        return; // reprojection failed :'(
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

bool ShouldSpawnProbe (uint2 TileIndex) {
    // Interleaved spawning
    return ((TileIndex.x + TileIndex.y) % 2) == UB.FrameIndex % 2;
}

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
            uint SpawnListRank = 0;
            InterlockedAdd(RWScreenProbeSpawnCountBuffer[0], 1, SpawnListRank);
            ScreenProbeHeader Header = (ScreenProbeHeader)0;
            Header.bValid = true;
            Header.PixelCoords = ScreenCoords;
            RWScreenProbeSpawnListBuffer[SpawnListRank] = PackProbeHeader(Header);
        }
    }
}

// Patch holes for reprojection dissoclusions 
[numthreads(WAVE_SIZE, 1, 1)]
void SubstituteScreenProbes (uint DispatchID : SV_DispatchThreadID) {
    uint FailListIndex = DispatchID;
    uint FailTileCount = RWReprojectionFailTileCountBuffer[0];
    if(FailListIndex >= FailTileCount) return;
    uint2 FailTileIndex = UnpackUint2x16(RWReprojectionFailTileListBuffer[FailListIndex]);
    uint SpawnProbeCount = RWScreenProbeSpawnCountBuffer[0];
    // Firstly try to append to the tail of the spawn list
    if(SpawnProbeCount + FailListIndex < UB.MaxProbesToSpawnPerFrame) {
        uint2 SubTileJitter = min(CalculateHaltonSequence(UB.FrameIndex) * TILE_SIZE, TILE_SIZE - 1.0f);
        ScreenProbeHeader Header = (ScreenProbeHeader)0;
        Header.bValid = true;
        Header.PixelCoords = FailTileIndex * TILE_SIZE + SubTileJitter;
        // Write the header to the spawn list
        RWScreenProbeSpawnListBuffer[SpawnProbeCount + FailListIndex] = PackProbeHeader(Header);
    } else {
        // Okay, we have too many probes to spawn, so we'll just substitute a previously spawned probe for update
        // Firstly permute the first elements
        uint SubstituteElementCount = UB.MaxProbesToSpawnPerFrame - SpawnProbeCount;
        uint SubstituteElementIndex = FailListIndex - SubstituteElementCount;
        if(SubstituteElementIndex < UB.MaxProbesToSpawnPerFrame) {
            // Pick a random probe from the suffix spawn list to substitute
            Random rng = MakeRandom(DispatchID, UB.FrameIndex);
            uint RandomElementIndex = SubstituteElementCount + rng.rand() * max(0, UB.MaxProbesToSpawnPerFrame - SubstituteElementCount);

        }
    }
    if(FailListIndex == 0) {
        // Specially, for the first thread
    }
}