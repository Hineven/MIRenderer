/*
 * Created: 2026/1/2
 * Author:  GitHub Copilot
 * See LICENSE for licensing.
 */

#include "rhi/rhi_cmd_stats.h"

MI_NAMESPACE_BEGIN

static const char* g_rhi_cmd_stat_names[] = {
    "Lambda",

    "ClearTexture",
    "ClearBuffer",
    "CopyBufferToTexture",
    "CopyTextureToBuffer",
    "CopyBuffer",
    "CopyTexture",
    "BlitTexture",

    "BeginRendering",
    "EndRendering",
    "UpdateDrawState",
    "Draw",
    "DrawIndexed",
    "DrawIndirect",
    "DrawIndexedIndirect",

    "SetScissor",
    "SetViewport",
    "SetCullMode",

    "Dispatch",
    "DispatchIndirect",

    "BindGraphicsPipeline",
    "BindComputePipeline",
    "CreateSignatureParameterTable",
    "BindSignatureParameterTable",
    "BindVertexBuffer",
    "PushConstants",

    "MemoryBarrier",
    "TextureBarrier",
    "BufferBarrier",
    "CombinedBarriers",
    "AccelerationStructureBarrier",

    "DebugMarkerBegin",
    "DebugMarkerEnd",
    "DebugMarkerInsert",
    "InsertTimestamp",

    "BuildAccelerationStructure",
    "BindRayTracingPipeline",
    "BindShaderBindingTable",
    "DispatchRays",
    "DispatchRaysIndirect",
    "DispatchRaysIndirect2",
};

static_assert((size_t)RHICmdStatId::kCount == sizeof(g_rhi_cmd_stat_names) / sizeof(g_rhi_cmd_stat_names[0]),
              "RHICmdStatId must match g_rhi_cmd_stat_names");

RHICmdStats& RHICmdStats::Get() {
    static RHICmdStats s;
    return s;
}

void RHICmdStats::AdvanceFrame(uint64_t frame_index) {
#if MI_ENABLE_RHI_CMD_STATS
    for (size_t i = 0; i < (size_t)RHICmdStatId::kCount; ++i) {
        last_frame_[i] = counters_[i].exchange(0, std::memory_order_relaxed);
    }
#else
    (void)frame_index;
#endif
    last_frame_index_.store(frame_index, std::memory_order_relaxed);
}

const char* RHICmdStats::GetName(RHICmdStatId id) {
    auto idx = (size_t)id;
    if (idx >= (size_t)RHICmdStatId::kCount) return "Unknown";
    return g_rhi_cmd_stat_names[idx];
}

MI_NAMESPACE_END

