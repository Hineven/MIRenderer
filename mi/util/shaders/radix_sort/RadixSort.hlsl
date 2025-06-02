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
StructuredBuffer<uint> Count;
RWStructuredBuffer<uint> OutKeys;
RWStructuredBuffer<uint> OutValues;

#define BINS_PER_GROUP 256
#define THREADS_PER_GROUP 256
#define ELEMENTS_PER_GROUP 1024

[numthreads(THREADS_PER_GROUP, 1, 1)]
void RadixSortScanShader (uint DispatchID : SV_DispatchThreadID) {
    
}