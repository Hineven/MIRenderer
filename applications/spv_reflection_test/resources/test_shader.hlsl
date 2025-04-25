//RWByteAddressBuffer MyBuffers[];

RWStructuredBuffer<uint> MyBuffers[];


[numthreads(1, 1, 1)]
void Main()
{
    //MyBuffers[1].Store(4, 12);
    MyBuffers[1][2] = 1;
    //RWBuffer<uint> buf = ResourceDescriptorHeap[1];
    //buf[1] = 1;
}