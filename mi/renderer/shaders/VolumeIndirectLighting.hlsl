// Volume probes
// Spawned from primary samples, with a global LRU queue
// Indexing structure: mipmapped atlas with a depth

// // Active probes in the previous frame
// StructuredBuffer<uint> PreviousActiveVolumeProbeCount;
// StructuredBuffer<uint> PreviousActiveVolumeProbeList;

// // All active probes.
// RWStructuredBuffer<uint> RWActiveVolumeProbeCount;
// RWStructuredBuffer<uint> RWActiveVolumeProbeList;

RWTexture2D<float4> RWVolumeProbeRadianceAtlasTexture;
// Probe header
RWTexture2D<uint4>  RWVolumeProbeHeaderTexture;

struct VolumeProbeHeader {
    float3 WorldPosition;
    bool bActive;
};

VolumeProbeHeader UnpackVolumeProbeHeader (uint4 HeaderData) {
    VolumeProbeHeader Header;
    Header.WorldPosition = HeaderData.xyz;
    Header.bActive = (HeaderData.w != 0);
    return Header;
}

uint4 PackVolumeProbeHeader (VolumeProbeHeader Header) {
    uint4 HeaderData;
    HeaderData.xyz = Header.WorldPosition;
    HeaderData.w = Header.bActive ? 1 : 0;
    return HeaderData;
}

// Indexing structure: per-tile lists of volume probe indices
RWStructuredBuffer<uint> RWTileVolumeProbeIndexListBuffer;
RWStructuredBuffer<uint> RWTileVolumeProbeIndexListLengthsBuffer;
RWStructuredBuffer<uint> RWTileVolumeProbeIndexListOffsetsBuffer;
RWStructuredBuffer<uint> RWTileVolumeProbeIndexListAllocator; // Used to allocate RWTileVolumeProbeIndexListBuffer entries to tiles

// Re-inject active probes from the previous frame to the tile index
[numthreads(WAVE_SIZE, 1, 1)]
void InjectProbes (uint DispatchID : SV_DispatchThreadID) {
    uint  ProbeIndex1 = DispatchID.x;
    uint2 ProbeIndex  = uint2(ProbeIndex1 % TileDimensions.x, ProbeIndex1 / TileDimensions.x);
    uint4 HeaderData = RWVolumeProbeHeaderTexture.Load(int3(ProbeIndex, 0, 0));
    VolumeProbeHeader Header = UnpackVolumeProbeHeader(HeaderData);

    
}

// Patch tiles without volume probes and will not spawn any in the next pass
[numthreads(WAVE_SIZE, 1, 1)]
void PatchVolumeProbes () {

} 

// Spawn new volume probes according to volume tile samples
[numthreads(WAVE_SIZE, 1, 1)]
void SpawnVolumeProbes () {

}

// Recover radiance for new probes & sample update rays for them
[numthreads(WAVE_SIZE, 1, 1)]
void RecoverProbeRadiance_SampleUpdateRays () {

}

// ... Same logic as those in IndirectLighting

// Ray radiances are traced, probes are updated. Time for shading!

// Filter probes
[numthreads(WAVE_SIZE, 1, 1)]
void FilterVolumeProbes () {

}

// Project probes to SH
[numthreads(WAVE_SIZE, 1, 1)]
void ProjectVolumeProbesToSH () {

}

// Shade volume with indirect lighting from probes
[numthreads(WAVE_SIZE, 1, 1)]
void ShadeVolumeProbes () {

}

// Reorder volume probes according to usage flags. Unused probes naturally fall behind and get evicted.
[numthreads(WAVE_SIZE, 1, 1)]
void ReorderVolumeProbes () {

}

