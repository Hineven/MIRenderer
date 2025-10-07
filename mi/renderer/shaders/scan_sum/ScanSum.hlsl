struct ScanSumUB {
    uint NumElements; // Number of elements to sort if non-indirect
    uint Padding2;
    uint Padding0;
    uint Padding1;
};

ConstantBuffer<ScanSumUB> UB;

StructuredBuffer<uint> Values;
StructuredBuffer<uint> BlockSums;
RWStructuredBuffer<uint> RWBlockSums;
StructuredBuffer<uint> Count;
RWStructuredBuffer<uint> RWOutValues;

#ifndef THREADS_PER_GROUP
#define THREADS_PER_GROUP 256
#endif

#ifndef ELEMENTS_PER_SEGMENT
#define ELEMENTS_PER_SEGMENT 1024
#endif

#ifndef WAVE_SIZE
#error "WAVE_SIZE must be defined"
#define WAVE_SIZE 32 // Make IDE linter happy
#endif

#if ELEMENTS_PER_SEGMENT % THREADS_PER_GROUP
#error "ELEMENTS_PER_SEGMENT must be a multiple of THREADS_PER_GROUP"
#endif

#if THREADS_PER_GROUP % WAVE_SIZE
#error "THREADS_PER_GROUP must be a multiple of WAVE_SIZE"
#endif

groupshared uint SharedSums[THREADS_PER_GROUP / WAVE_SIZE];

#if THREADS_PER_GROUP / WAVE_SIZE > WAVE_SIZE
#error "THREADS_PER_GROUP / WAVE_SIZE must be <= WAVE_SIZE"
#endif

[numthreads(THREADS_PER_GROUP, 1, 1)]
void ScanSumBlockSum (uint LocalID : SV_GroupThreadID, uint GroupID : SV_GroupID) {
    // Scan each group of elements.

#ifndef SCAN_SUM_INDIRECT
    uint NumElements = UB.NumElements;
#else
    uint NumElements = Count[0];
#endif
    // Sum the segment
    uint Sum = 0;
    uint StartOffset = GroupID * ELEMENTS_PER_SEGMENT;
    for(uint i = 0; i < ELEMENTS_PER_SEGMENT; i += THREADS_PER_GROUP) {
        uint Index = StartOffset + i + LocalID;
        if (Index < NumElements) {
            Sum += Values[Index];
        }
    }
    Sum = WaveActiveSum(Sum);
    if(WaveIsFirstLane()) {
        SharedSums[LocalID / WAVE_SIZE]  = Sum;
    }
    GroupMemoryBarrierWithGroupSync();
    // Second sum (assume that THREADS_PER_GROUP / WAVE_SIZE <= WAVE_SIZE)
    if(LocalID < THREADS_PER_GROUP / WAVE_SIZE) {
        uint WaveSum = SharedSums[LocalID];
        WaveSum = WaveActiveSum(WaveSum);
        if(WaveIsFirstLane()) {
            RWBlockSums[GroupID] = WaveSum;
        }
    }
}

#if THREADS_PER_GROUP / WAVE_SIZE > WAVE_SIZE
#error "THREADS_PER_GROUP / WAVE_SIZE must be <= WAVE_SIZE"
#endif

groupshared uint SharedAccumulatedPrefixSum;
[numthreads(THREADS_PER_GROUP, 1, 1)]
void ScanSumSumBlockSums (uint LocalID : SV_GroupThreadID) {
#ifndef SCAN_SUM_INDIRECT
    uint NumElements = UB.NumElements;
#else
    uint NumElements = Count[0];
#endif
    // Prefix sum the block sums
    uint NumBlocks = (NumElements + ELEMENTS_PER_SEGMENT - 1) / ELEMENTS_PER_SEGMENT;
    if(LocalID == 0) {
        SharedAccumulatedPrefixSum = 0;
    }
    for(int BaseBlock = 0; BaseBlock < NumBlocks; BaseBlock += THREADS_PER_GROUP) {
        uint BlockIndex = BaseBlock + LocalID;
        uint BlockSum = 0;
        if(BlockIndex < NumBlocks) {
            BlockSum = RWBlockSums[BlockIndex];
        }
        uint ExclusiveWaveInnerBlockSum = WavePrefixSum(BlockSum);
        GroupMemoryBarrierWithGroupSync();
        if(WaveGetLaneIndex() == (WAVE_SIZE - 1)) {
            // Inclusive inter-wave sum
            SharedSums[LocalID / WAVE_SIZE]  = ExclusiveWaveInnerBlockSum + BlockSum;
        }
        GroupMemoryBarrierWithGroupSync();
        uint ExclusiveWaveInterSums = 0;
        if(LocalID < THREADS_PER_GROUP / WAVE_SIZE) {
            // The first wave is responsible for summing the wave sums
            ExclusiveWaveInterSums = SharedSums[LocalID];
            ExclusiveWaveInterSums = WavePrefixSum(ExclusiveWaveInterSums);
        }
        GroupMemoryBarrierWithGroupSync();
        if(LocalID < THREADS_PER_GROUP / WAVE_SIZE) {
            // Exclusive inter-wave sum
            SharedSums[LocalID] = ExclusiveWaveInterSums;
        }
        GroupMemoryBarrierWithGroupSync();
        // Inclusive in-group prefix sum
        uint GroupInnerPrefixSum = SharedSums[LocalID / WAVE_SIZE] + ExclusiveWaveInnerBlockSum + BlockSum;
        // Add the accumulated prefix sum from previous groups
        uint FullPrefixSum = GroupInnerPrefixSum + SharedAccumulatedPrefixSum;
        if(BlockIndex < NumBlocks) RWBlockSums[BlockIndex] = FullPrefixSum;
        GroupMemoryBarrierWithGroupSync();
        if(LocalID == THREADS_PER_GROUP - 1) {
            SharedAccumulatedPrefixSum = FullPrefixSum;
        }
    }
}

[numthreads(THREADS_PER_GROUP, 1, 1)]
void ScanSumScatterBlockSums (uint LocalID : SV_GroupThreadID, uint GroupID : SV_GroupID) {
#ifndef SCAN_SUM_INDIRECT
    uint NumElements = UB.NumElements;
#else
    uint NumElements = Count[0];
#endif
    uint StartOffset = GroupID * ELEMENTS_PER_SEGMENT;
    uint BlockPrefixSum = 0;
    if(GroupID > 0) {
        BlockPrefixSum = BlockSums[GroupID - 1];
    }
    if(LocalID == 0) {
        SharedAccumulatedPrefixSum = 0;
    }
    for(uint i = 0; i < ELEMENTS_PER_SEGMENT && i + StartOffset < NumElements; i += THREADS_PER_GROUP) {
        uint Index = StartOffset + i + LocalID;
        uint Value = 0;
        if (Index < NumElements) {
            Value = Values[Index];
        }
        uint ExclusiveWaveInnerPrefixSum = WavePrefixSum(Value);
        GroupMemoryBarrierWithGroupSync();
        if(WaveGetLaneIndex() == (WAVE_SIZE - 1)) {
            SharedSums[LocalID / WAVE_SIZE]  = ExclusiveWaveInnerPrefixSum + Value;
        }
        GroupMemoryBarrierWithGroupSync();
        uint ExclusiveWaveInterSums = 0;
        if(LocalID < THREADS_PER_GROUP / WAVE_SIZE) { // The first wave is responsible for summing the wave sums
            uint WaveInnerSums = SharedSums[LocalID];
            ExclusiveWaveInterSums = WavePrefixSum(WaveInnerSums);
        }
        GroupMemoryBarrierWithGroupSync();
        if(LocalID < THREADS_PER_GROUP / WAVE_SIZE) {
            SharedSums[LocalID] = ExclusiveWaveInterSums;
        }
        GroupMemoryBarrierWithGroupSync();
        uint GroupInnerPrefixSum = SharedSums[LocalID / WAVE_SIZE] + ExclusiveWaveInnerPrefixSum + Value;
        uint FullPrefixSum = GroupInnerPrefixSum + SharedAccumulatedPrefixSum;

        if(Index < NumElements) RWOutValues[Index] = BlockPrefixSum + FullPrefixSum;
        GroupMemoryBarrierWithGroupSync();

        if(LocalID == THREADS_PER_GROUP - 1) {
            SharedAccumulatedPrefixSum = FullPrefixSum;
        }
    }
}