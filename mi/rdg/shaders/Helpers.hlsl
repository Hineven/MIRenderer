
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
}

struct SpawnTraceRaysIndirectCommand1DUB {
    uint64_t RaygenAddr;
    uint64_t RaygenSize;
    uint64_t MissAddr;
    uint64_t MissSize;
    uint64_t MissStride;
    uint64_t HitAddr;
    uint64_t HitSize;
    uint64_t HitStride;
};

#if GRAPHICS_API == 0
struct TraceRaysIndirectCommand {
    uint32_t Width;
    uint32_t Height;
    uint32_t Depth;
    uint32_t Padding;
};
struct TraceRaysIndirectCommand2 {
    uint64_t RaygenAddr;
    uint64_t RaygenSize;
    uint64_t MissAddr;
    uint64_t MissSize;
    uint64_t MissStride;
    uint64_t HitAddr;
    uint64_t HitSize;
    uint64_t HitStride;
    uint64_t CallableAddr;
    uint64_t CallableSize;
    uint64_t CallableStride;
    uint32_t Width;
    uint32_t Height;
    uint32_t Depth;
};
#else
#error "Unsupported graphics API for TraceRaysIndirectCommand"
#endif

ConstantBuffer<SpawnTraceRaysIndirectCommand1DUB> SpawnTraceRaysIndirectCommand1D_UB;

#ifdef TRACE_RAYS_2
RWStructuredBuffer<TraceRaysIndirectCommand2> SpawnTraceRaysIndirectCommand1D_Command;
#else
RWStructuredBuffer<TraceRaysIndirectCommand> SpawnTraceRaysIndirectCommand1D_Command;
#endif
[numthreads(1, 1, 1)]
void SpawnTraceRaysIndirectCommand1D() {
#ifdef TRACE_RAYS_2
    TraceRaysIndirectCommand2 Cmd = (TraceRaysIndirectCommand2)0;
    Cmd.RaygenAddr = SpawnTraceRaysIndirectCommand1D_UB.RaygenAddr;
    Cmd.RaygenSize = SpawnTraceRaysIndirectCommand1D_UB.RaygenSize;
    Cmd.MissAddr = SpawnTraceRaysIndirectCommand1D_UB.MissAddr;
    Cmd.MissSize = SpawnTraceRaysIndirectCommand1D_UB.MissSize;
    Cmd.MissStride = SpawnTraceRaysIndirectCommand1D_UB.MissStride;
    Cmd.HitAddr = SpawnTraceRaysIndirectCommand1D_UB.HitAddr;
    Cmd.HitSize = SpawnTraceRaysIndirectCommand1D_UB.HitSize;
    Cmd.HitStride = SpawnTraceRaysIndirectCommand1D_UB.HitStride;
#else
    TraceRaysIndirectCommand Cmd = (TraceRaysIndirectCommand)0;
#endif
    // No callables for now
    Cmd.Width = Count[0];
    Cmd.Height = 1;
    Cmd.Depth = 1;
    SpawnTraceRaysIndirectCommand1D_Command[0] = Cmd;
}