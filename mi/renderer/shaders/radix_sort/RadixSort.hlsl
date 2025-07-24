struct RadixSortUB {
    uint NumElements; // Number of elements to sort if non-indirect
    uint BitShift; // Bit shift for the current pass
    uint Padding0;
    uint Padding1;
};

ConstantBuffer<RadixSortUB> UB;

StructuredBuffer<uint> Keys;
StructuredBuffer<uint> Values;
StructuredBuffer<uint> Bins;
RWStructuredBuffer<uint> RWBins;
StructuredBuffer<uint> SumBins;
RWStructuredBuffer<uint> RWSumBins;
StructuredBuffer<uint> Count;
RWStructuredBuffer<uint> RWOutKeys;
RWStructuredBuffer<uint> RWOutValues;

#define BINS_PER_PASS 256 // As well as threads per group
#define ELEMENTS_PER_SEGMENT 1024

#ifndef WAVE_SIZE
#error "WAVE_SIZE must be defined"
#define WAVE_SIZE 32
#endif

#if ELEMENTS_PER_SEGMENT % BINS_PER_PASS
#error "ELEMENTS_PER_SEGMENT must be a multiple of BINS_PER_PASS"
#endif

#if BINS_PER_PASS % WAVE_SIZE
#error "BINS_PER_PASS must be a multiple of WAVE_SIZE"
#endif



groupshared uint SharedBins[BINS_PER_PASS];
[numthreads(BINS_PER_PASS, 1, 1)]
void RadixSortScan (uint LocalID : SV_GroupThreadID, uint GroupID : SV_GroupID) {
    // Scan each group of elements. Prefix sum within groups and store in Bins.

    // Clear groupshared memory
    SharedBins[LocalID] = 0;

#ifndef RADIX_SORT_INDIRECT
    uint NumElements = UB.NumElements;
#else
    uint NumElements = Count[0];
#endif

    GroupMemoryBarrierWithGroupSync();

    // Count the segment
    uint StartOffset = GroupID * ELEMENTS_PER_SEGMENT;
    for(uint i = 0; i < ELEMENTS_PER_SEGMENT; i += BINS_PER_PASS) {
        uint Index = StartOffset + i + LocalID;
        if (Index < NumElements) {
            uint Key = Keys[Index];
            uint BinIndex = (Key >> UB.BitShift) & (BINS_PER_PASS - 1);
            InterlockedAdd(SharedBins[BinIndex], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    
    uint NumSegments = (NumElements + ELEMENTS_PER_SEGMENT - 1) / ELEMENTS_PER_SEGMENT;

    // Write the results back to the Bins buffer
    RWBins[LocalID * NumSegments + GroupID] = SharedBins[LocalID];
}

// One single thread group for summing one kind of bin (256 groups)
#define SUM_ARRAY_SIZE 256
groupshared uint SharedSumArray[SUM_ARRAY_SIZE];
[numthreads(SUM_ARRAY_SIZE, 1, 1)]
void RadixSortSum (uint LocalID : SV_GroupThreadID, uint GroupID : SV_GroupID) {
#ifndef RADIX_SORT_INDIRECT
    uint NumSegments = (UB.NumElements + ELEMENTS_PER_SEGMENT - 1) / ELEMENTS_PER_SEGMENT;
#else
    uint NumSegments = (Count[0] + ELEMENTS_PER_SEGMENT - 1) / ELEMENTS_PER_SEGMENT;
#endif
    // Each thread group sums the bins for a segment of elements.
    uint CurrentPrefixSum = 0;
    for(uint StartOffset = 0; StartOffset < NumSegments; StartOffset += SUM_ARRAY_SIZE) {
        // Process SUM_ARRAY_SIZE elements at once
        // Load the bins for the current segment
        {
            uint SegmentIndex = StartOffset + LocalID;
            uint Value = 0;
            if(SegmentIndex < NumSegments) {
                Value = RWBins[SegmentIndex + GroupID * NumSegments];
            }
            SharedSumArray[LocalID] = Value;
        }
        GroupMemoryBarrierWithGroupSync();
        // Prefix sum the bins
        uint Temp = SharedSumArray[LocalID];
        for(uint Level = 1; Level < SUM_ARRAY_SIZE; Level *= 2) {
            if(LocalID >= Level) {
                Temp += SharedSumArray[LocalID - Level];
            }
            GroupMemoryBarrierWithGroupSync();
            SharedSumArray[LocalID] = Temp;
            GroupMemoryBarrierWithGroupSync();
        }
        // Write the prefix sum back to the Bins buffer
        {
            uint SegmentIndex = StartOffset + LocalID;
            if(SegmentIndex < NumSegments) {
                RWBins[SegmentIndex + GroupID * NumSegments] = SharedSumArray[LocalID] + CurrentPrefixSum;
            }
        }
        // Update the current prefix sum
        CurrentPrefixSum += SharedSumArray[SUM_ARRAY_SIZE - 1];
        GroupMemoryBarrierWithGroupSync();
    }
}

// Prefix sum the summed bins. Only one group here.
[numthreads(BINS_PER_PASS, 1, 1)]
void RadixSortSumBins (uint LocalID : SV_GroupThreadID) {
#ifndef RADIX_SORT_INDIRECT
    uint NumSegments = (UB.NumElements + ELEMENTS_PER_SEGMENT - 1) / ELEMENTS_PER_SEGMENT;
#else
    uint NumSegments = (Count[0] + ELEMENTS_PER_SEGMENT - 1) / ELEMENTS_PER_SEGMENT;
#endif
    // Load the sums of bins
    {
        SharedBins[LocalID] = Bins[LocalID * NumSegments + NumSegments - 1];
    }
    GroupMemoryBarrierWithGroupSync();
    // Prefix sum the bins
    uint Temp = SharedBins[LocalID];
    for(uint Level = 1; Level < BINS_PER_PASS; Level *= 2) {
        if(LocalID >= Level) {
            Temp += SharedBins[LocalID - Level];
        }
        GroupMemoryBarrierWithGroupSync();
        SharedBins[LocalID] = Temp;
        GroupMemoryBarrierWithGroupSync();
    }
    // Write the prefix sum back to the Bins buffer
    {
        RWSumBins[LocalID] = SharedBins[LocalID];
    }
}

#if WAVE_SIZE <= 32
#define WaveMask_T uint
#else
// for AMD devices
#define WaveMask_T uint64_t
#endif
groupshared WaveMask_T SharedBinsMask[BINS_PER_PASS];
// Finally, reorder. Each thread group cooperatively reorders a segment of elements.
[numthreads(WAVE_SIZE, 1, 1)]
void RadixSortScatter (uint LocalID : SV_GroupThreadID, uint GroupID : SV_GroupID) {
#ifndef RADIX_SORT_INDIRECT
    uint NumElements = UB.NumElements;
#else
    uint NumElements = Count[0];
#endif
    uint NumSegments = (NumElements + ELEMENTS_PER_SEGMENT - 1) / ELEMENTS_PER_SEGMENT;

    uint StartOffset = GroupID * ELEMENTS_PER_SEGMENT;
    // Clear groupshared memory (mask)
    for(uint i = 0; i < BINS_PER_PASS; i += WAVE_SIZE) {
        SharedBinsMask[i + LocalID] = 0;
    }

    GroupMemoryBarrierWithGroupSync();

    // Load the bins into groupshared memory
    for(uint Offset = 0; Offset < BINS_PER_PASS; Offset += WAVE_SIZE) {
        uint Prefix = 0;
        if(Offset + LocalID > 0) Prefix = SumBins[Offset + LocalID - 1];
        SharedBins[Offset + LocalID] = Prefix + Bins[(Offset + LocalID) * NumSegments + GroupID];
    }
    
    GroupMemoryBarrierWithGroupSync();

    // Reorder the elements
    for(int Offset = ELEMENTS_PER_SEGMENT; Offset > 0; Offset -= WAVE_SIZE) {
        int Index = StartOffset + Offset - WAVE_SIZE + LocalID;
        if (Index < NumElements) {
            uint Key = Keys[Index];
            uint BinIndex = (Key >> UB.BitShift) & (BINS_PER_PASS - 1);
            InterlockedOr(SharedBinsMask[BinIndex], WaveMask_T(1) << LocalID);
            GroupMemoryBarrierWithGroupSync();
            WaveMask_T Threads = SharedBinsMask[BinIndex];
            WaveMask_T Masked = Threads & (~((WaveMask_T(1) << LocalID) - 1));
            uint Rank = countbits(Masked);
            uint ReorderIndex = SharedBins[BinIndex] - Rank;
            // Reorder
            RWOutKeys[ReorderIndex] = Key;
            RWOutValues[ReorderIndex] = Values[Index];
            // Update the bin count
            bool bIsPrimary = Masked == Threads;
            GroupMemoryBarrierWithGroupSync();
            if (bIsPrimary) {
                SharedBins[BinIndex] -= Rank;
                SharedBinsMask[BinIndex] = 0; // Reset the mask for the next pass
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
}