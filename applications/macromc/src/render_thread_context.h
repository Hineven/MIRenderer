/*
 * Created: 2026/06/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_RENDER_THREAD_CONTEXT_H
#define MACROMC_RENDER_THREAD_CONTEXT_H

#include <atomic>
#include <future>
#include <functional>
#include <memory>
#include <thread>

#include <glm/glm.hpp>

#include "core/thr.h"
#include "core/refcounted.h"
#include "rhi/rhi.h"
#include "render_command.h"
#include "gigavoxel_shell_registry.h"

MACROMC_NAMESPACE_BEGIN

// =============================================================================
// RenderThreadContext — owns the render-thread side of the gameplay/render
// split (THREAD_MODEL.md).
//
// Responsibilities:
//   - Own the SPSC render-command queue (RenderCommandQueue). The gameplay
//     thread is the single producer (EnqueueCommand); the render thread is the
//     single consumer (drained each frame in the render loop).
//   - Own the frame-overlap sync primitives (previous_frame_future_ /
//     previous_frame_sync_point_), migrated here from MacroMCApp so the render
//     thread is the sole owner of frame-advance state.
//   - Own a render-side camera snapshot, updated from UpdateCameraCmd. The
//     gameplay thread never shares its live camera; it pushes an
//     UpdateCameraCmd each tick and the render thread paints from the latest
//     received snapshot.
//   - Run the render loop (its own std::thread when not in bypass mode).
//
// Bypass mode (MACROMC_BYPASS_RENDER_THREAD): EnqueueCommand executes the
// command inline on the calling thread, no render thread is spawned, and
// the frame-advance is driven synchronously by the caller via TickRender().
// This collapses the three-thread model back to single-threaded for debugging.
// =============================================================================

// A render-side snapshot of the camera, updated from UpdateCameraCmd.
// RenderFrame reads from this, never from the gameplay thread's view_.
struct RenderCameraSnapshot {
    glm::vec3 position {0, 0, 0};
    glm::vec3 forward  {0, 0, -1};
    glm::vec3 up       {0, 1, 0};
    float fov_y_rad = 1.0472f;  // 60 deg default
};

class RenderThreadContext {
public:
    RenderThreadContext() = default;
    ~RenderThreadContext();

    RenderThreadContext(const RenderThreadContext&) = delete;
    RenderThreadContext& operator=(const RenderThreadContext&) = delete;

    // Set up: create the sync point, install the render-frame callback (called
    // by the render loop to actually paint — the app binds it to its
    // RendererView/Renderer/scene), and create the GigaVoxelShellRegistry bound
    // to the given scene. `bypass` controls whether a real render thread is
    // spawned (false) or commands run inline (true).
    void Initialize(mi::RHI& rhi, mi::Scene * scene,
                    std::function<void()> render_frame_fn, bool bypass);

    // Access the shell registry (render-thread side). Used by the app's render
    // callback to push mesh data produced on the render thread directly (e.g.
    // flattening meshing results), bypassing the SPSC queue.
    GigaVoxelShellRegistry& ShellRegistry() { return *shell_registry_; }

    // Start the render thread (no-op in bypass mode). Call after Initialize.
    void Start();
    // Signal the render thread to stop and join it. Safe to call from main.
    void Stop();

    // --- Producer side (gameplay thread) ---
    // Enqueue a command for the render thread. In bypass mode, executes the
    // command immediately inline (single-threaded). Returns false only if the
    // SPSC ring is full (non-bypass) — the caller should retry next tick;
    // commands must not be dropped (a lost UploadChunkMesh leaks a chunk).
    bool EnqueueCommand(RenderCommand cmd);

    // --- Consumer side (render thread, or main in bypass) ---
    // Drain all queued commands, applying them to the render-side state
    // (camera snapshot, future GigaVoxel renderable state). Called at the top
    // of each rendered frame.
    void DrainCommands();

    // --- Frame advance (render thread, or main in bypass) ---
    // Renders one frame via the installed callback, then does the frame-overlap
    // wait + AdvanceFrame. In bypass this is called directly by the app's
    // single-threaded Run loop; otherwise it's called by the render thread loop.
    void TickRender();

    // The render-side camera snapshot. RenderFrame reads this.
    RenderCameraSnapshot& Camera() { return camera_; }
    const RenderCameraSnapshot& Camera() const { return camera_; }

    RenderCommandQueue& Queue() { return command_queue_; }

private:
    void RenderThreadLoop();
    void ApplyCommand(const RenderCommand& cmd);

    mi::RHI* rhi_ = nullptr;
    std::function<void()> render_frame_fn_;
    bool bypass_ = true;

    RenderCommandQueue command_queue_;
    RenderCameraSnapshot camera_;
    // Render-thread-side GigaVoxel asset owner. Driven by RenderCommand dispatch
    // (ApplyCommand routes shell-level commands here; camera is handled inline).
    std::unique_ptr<GigaVoxelShellRegistry> shell_registry_;

    // Frame overlap (owned exclusively by the render side).
    std::future<void> previous_frame_future_;
    mi::TRef<mi::RHISyncPoint> previous_frame_sync_point_;

    // Render thread (non-bypass only).
    std::thread render_thread_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
};

MACROMC_NAMESPACE_END

#endif // MACROMC_RENDER_THREAD_CONTEXT_H
