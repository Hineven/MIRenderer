/*
 * Created: 2026/06/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "render_thread_context.h"

#include <chrono>
#include <utility>

#include "core/thr.h"
#include "core/platform.h"

MACROMC_NAMESPACE_BEGIN

RenderThreadContext::~RenderThreadContext() {
    Stop();
}

void RenderThreadContext::Initialize(mi::RHI& rhi, mi::Scene * scene,
                                     std::function<void()> render_frame_fn,
                                     bool bypass) {
    rhi_ = &rhi;
    render_frame_fn_ = std::move(render_frame_fn);
    bypass_ = bypass;
    if (rhi_) {
        previous_frame_sync_point_ = rhi_->CreateSyncPoint();
    }
    // The shell registry owns the GigaVoxel assets on the render side. It is
    // driven by command dispatch from ApplyCommand (shell-level commands route
    // here; camera is handled inline).
    shell_registry_ = std::make_unique<GigaVoxelShellRegistry>(scene);
}

void RenderThreadContext::Start() {
    if (!bypass_ && !running_.load()) {
        stop_requested_.store(false);
        running_.store(true);
        render_thread_ = std::thread([this] { RenderThreadLoop(); });
    }
    // bypass: no thread; TickRender/DrainCommands are called inline by the app.
}

void RenderThreadContext::Stop() {
    if (!running_.load()) return;
    stop_requested_.store(true);
    if (render_thread_.joinable()) render_thread_.join();
    running_.store(false);
}

bool RenderThreadContext::EnqueueCommand(RenderCommand cmd) {
    if (bypass_) {
        // Single-threaded: apply immediately, no queue hop.
        ApplyCommand(cmd);
        return true;
    }
    return command_queue_.Push(std::move(cmd));
}

void RenderThreadContext::DrainCommands() {
    RenderCommand cmd;
    while (command_queue_.Pop(cmd)) {
        ApplyCommand(cmd);
    }
}

void RenderThreadContext::ApplyCommand(const RenderCommand& cmd) {
    std::visit([this](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, UpdateCameraCmd>) {
            // Camera is owned by the RTC itself (it's per-view, not per-shell).
            camera_.position = c.position;
            camera_.forward = c.forward;
            camera_.up = c.up;
            camera_.fov_y_rad = c.fov_y_rad;
        } else if constexpr (std::is_same_v<T, CreateGigaVoxelShellCmd>) {
            if (shell_registry_) shell_registry_->HandleCommand(c);
        } else if constexpr (std::is_same_v<T, DestroyGigaVoxelShellCmd>) {
            if (shell_registry_) shell_registry_->HandleCommand(c);
        } else if constexpr (std::is_same_v<T, UploadChunkMeshCmd>) {
            if (shell_registry_) shell_registry_->HandleCommand(c);
        } else if constexpr (std::is_same_v<T, DestroyChunkMeshCmd>) {
            if (shell_registry_) shell_registry_->HandleCommand(c);
        }
    }, cmd);
}

void RenderThreadContext::TickRender() {
    // 1. Apply any queued gameplay commands (camera, chunk uploads, etc.).
    //    In bypass mode the queue is empty (commands were applied inline by
    //    EnqueueCommand), but DrainCommands is still cheap (one failed Pop).
    DrainCommands();

    // 2. Paint. The callback is bound by the app to its RendererView/Renderer.
    if (render_frame_fn_) render_frame_fn_();

    // 3. Frame-overlap sync + advance. Mirrors the ViewerApp pattern: wait for
    //    the frame-before-last to finish (host future + GPU fence), then submit
    //    this frame. See THREAD_MODEL.md and viewer_app.cpp.
    if (!rhi_) return;
    if (previous_frame_future_.valid()) previous_frame_future_.wait();
    if (previous_frame_sync_point_) {
        previous_frame_sync_point_->Wait();
        previous_frame_sync_point_->Reset();
    }
    previous_frame_future_ = rhi_->AdvanceFrame(previous_frame_sync_point_.Raw());
}

void RenderThreadContext::RenderThreadLoop() {
    // The render thread marks itself as the render thread (the RHI layer
    // requires this: RenderGraph::Execute / AdvanceFrame / SyncPoint::Wait all
    // assert IsRenderThread()). See mi/rdg/rdg.cpp:108, mi/rhi/vk/vk_resource.cpp.
    mi::SetCurrentThreadType(mi::ThreadType::kRenderThread);
    InitializePlatformBackgroundThreadContext_Worker();

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        TickRender();
        // Free-running render loop. Real frame-pacing (vsync / target FPS cap)
        // lands with the GigaVoxel rendering phase; for now this spins as fast
        // as the GPU lets it (AdvanceFrame blocks on the overlap sync).
    }

    mi::SetCurrentThreadType(mi::ThreadType::kUnknown);
}

MACROMC_NAMESPACE_END
