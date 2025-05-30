
struct DispatchIndirecCommand
{
    uint ThreadGroupCountX;
    uint ThreadGroupCountY;
    uint ThreadGroupCountZ;
    uint Padding;
};

RWStructuredBuffer<DispatchIndirecCommand> Command;
StructuredBuffer<uint> Count;

[numthreads(1, 1, 1)]
void SpawnDispatchIndirectCommand1D () {
    DispatchIndirecCommand Cmd = (DispatchIndirecCommand)0;
    Cmd.ThreadGroupCountX = Count[0];
    Command[0] = Cmd;
}