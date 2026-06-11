/*
 * Created: 2026/1/2
 * Author:  GitHub Copilot
 * See LICENSE for licensing.
 *
 * RHI command statistics (per-frame counters).
 */

#ifndef MIRENDERERDEV_RHI_CMD_STATS_H
#define MIRENDERERDEV_RHI_CMD_STATS_H

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "core/base.h"

MI_NAMESPACE_BEGIN

// Enable/disable command stats globally.
// Default: enabled in debug, disabled in release.
#ifndef MI_ENABLE_RHI_CMD_STATS
    #if !defined(NDEBUG)
        #define MI_ENABLE_RHI_CMD_STATS 1
    #else
        #define MI_ENABLE_RHI_CMD_STATS 0
    #endif
#endif

// Unique ids for each RHICommand type we want to count.
// Keep this in sync with g_rhi_cmd_stat_names and instrumentation sites.
enum class RHICmdStatId : uint16_t {
    kLambda = 0,

    kClearTexture,
    kClearBuffer,
    kCopyBufferToTexture,
    kCopyTextureToBuffer,
    kCopyBuffer,
    kCopyTexture,
    kBlitTexture,

    kBeginRendering,
    kEndRendering,
    kUpdateDrawState,
    kDraw,
    kDrawIndexed,
    kDrawIndirect,
    kDrawIndexedIndirect,

    kSetScissor,
    kSetViewport,
    kSetCullMode,

    kDispatch,
    kDispatchIndirect,

    kBindGraphicsPipeline,
    kBindComputePipeline,
    kCreateSignatureParameterTable,
    kBindSignatureParameterTable,
    kBindVertexBuffer,
    kPushConstants,

    kMemoryBarrier,
    kTextureBarrier,
    kBufferBarrier,
    kCombinedBarriers,
    kAccelerationStructureBarrier,

    kDebugMarkerBegin,
    kDebugMarkerEnd,
    kDebugMarkerInsert,
    kInsertTimestamp,

    // Ray tracing
    kBuildAccelerationStructure,
    kBindRayTracingPipeline,
    kBindShaderBindingTable,
    kDispatchRays,
    kDispatchRaysIndirect,
    kDispatchRaysIndirect2,

    kCount,
};

struct RHICmdStatRecord {
    RHICmdStatId id;
    uint64_t count;
};

// Thread-safe global counter set. We aggregate counts across all queues.
class RHICmdStats {
public:
    static RHICmdStats& Get();

    // Called when a command is executed on the RHI thread.
    FORCEINLINE void Increment(RHICmdStatId id) {
#if MI_ENABLE_RHI_CMD_STATS
        counters_[(size_t)id].fetch_add(1, std::memory_order_relaxed);
#else
        (void)id;
#endif
    }

    FORCEINLINE static bool IsEnabled() {
#if MI_ENABLE_RHI_CMD_STATS
        return true;
#else
        return false;
#endif
    }

    // Rotate counters for a new frame.
    // After this call, GetLastFrameCounters() will refer to the frame that just ended.
    void AdvanceFrame(uint64_t frame_index);

    // Snapshot of last frame counters. Safe to read from any thread.
    std::span<const uint64_t> GetLastFrameCounters() const {
        return std::span<const uint64_t>(last_frame_.data(), last_frame_.size());
    }

    uint64_t GetLastFrameIndex() const { return last_frame_index_.load(std::memory_order_relaxed); }

    // Names for debug printing / UI.
    static const char* GetName(RHICmdStatId id);

private:
    RHICmdStats() = default;

#if MI_ENABLE_RHI_CMD_STATS
    std::array<std::atomic<uint64_t>, (size_t)RHICmdStatId::kCount> counters_{};
    std::array<uint64_t, (size_t)RHICmdStatId::kCount> last_frame_{};
    std::atomic<uint64_t> last_frame_index_{0};
#else
    // Keep layout minimal when disabled.
    std::array<uint64_t, (size_t)RHICmdStatId::kCount> last_frame_{};
    std::atomic<uint64_t> last_frame_index_{0};
#endif
};

#if MI_ENABLE_RHI_CMD_STATS
    #define MI_RHI_CMD_STAT_INC(ID) ::mi::RHICmdStats::Get().Increment((ID))
#else
    #define MI_RHI_CMD_STAT_INC(ID) do { (void)(ID); } while(0)
#endif

MI_NAMESPACE_END

#endif // MIRENDERERDEV_RHI_CMD_STATS_H

