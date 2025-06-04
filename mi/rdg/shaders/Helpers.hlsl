
struct DispatchIndirecCommand
{
    uint ThreadGroupCountX;
    uint ThreadGroupCountY;
    uint ThreadGroupCountZ;
    uint Padding;
};

struct SpawnDispatchIndirectCommand1DUB {
    uint32_t UpDivisor; // The divisor for calculating the number of dispatches
    uint32_t Padding0;
    uint32_t Padding1;
    uint32_t Padding2;
};

ConstantBuffer<SpawnDispatchIndirectCommand1DUB> UB;

RWStructuredBuffer<DispatchIndirecCommand> Command;
StructuredBuffer<uint> Count;

[numthreads(1, 1, 1)]
void SpawnDispatchIndirectCommand1D () {
    DispatchIndirecCommand Cmd = (DispatchIndirecCommand)0;
    uint Divisor = max(UB.UpDivisor, 1);
    Cmd.ThreadGroupCountX = (Count[0] + Divisor - 1) / Divisor;
    Cmd.ThreadGroupCountY = 1;
    Cmd.ThreadGroupCountZ = 1;
    Command[0] = Cmd;
    printf("SpawnDispatchIndirectCommand1D: %d\n", Cmd.ThreadGroupCountX);
}