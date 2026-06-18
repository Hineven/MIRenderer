/*
 * Created: 2026/06/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_RENDER_COMMAND_H
#define MACROMC_RENDER_COMMAND_H

#include <variant>
#include <vector>

#include <glm/glm.hpp>

#include "core/util/queue.h"
#include "renderer/mi_giga_voxel.h"
#include "world/types.h"

MACROMC_NAMESPACE_BEGIN

// =============================================================================
// RenderCommand — gameplay → render thread cross-thread commands.
//
// The gameplay thread is the single producer; the render thread is the single
// consumer. Commands flow through a lock-free SPSC ring (RenderCommandQueue)
// and are drained at the start of each render frame. See THREAD_MODEL.md §4.
//
// The gameplay thread NEVER touches RHI. Anything that needs to happen on the
// GPU is expressed as a command here and executed by the render thread.
//
// Payloads carry only plain data (coords, flattened mesh vectors, host-data) —
// never TRef<RHIResource>. The render thread owns all RHI resources exclusively.
//
// ShellId tags every chunk-level command so the render-side
// GigaVoxelShellRegistry routes it to the right GigaVoxel asset (one asset per
// shell; see THREAD_MODEL.md §7).
// =============================================================================

// Update the render-side camera snapshot from the gameplay-side camera.
// Sent every gameplay tick (the render thread paints from the latest received
// snapshot; missing a beat just means it reuses the previous one).
struct UpdateCameraCmd {
    glm::vec3 position;
    glm::vec3 forward;
    glm::vec3 up;
    float fov_y_rad = 0.0f;
};

// Create a GigaVoxel asset for a shell and register it in the scene. The atlas
// is a global config (set once at app init via GigaVoxel::SetGlobalAtlas), so
// no atlas data is carried here. One shell maps to one GigaVoxel asset.
struct CreateGigaVoxelShellCmd {
    ShellId shell = kInvalidShellId;
    ShellCategory category = ShellCategory::kEmpty;
};

// Destroy a shell's GigaVoxel asset + instance (releases GPU resources via the
// render thread's delayed-free path).
struct DestroyGigaVoxelShellCmd {
    ShellId shell = kInvalidShellId;
};

// Upload / refresh a chunk's mesh data into the shell's GigaVoxel asset. The
// mesh data (already flattened + converted to GigaVoxelVertex on the render
// thread side, since meshing results only become readable there) is handed off:
// after this command executes, the producer no longer holds it. Indices are
// chunk-local (0-based).
struct UploadChunkMeshCmd {
    ShellId shell = kInvalidShellId;
    ChunkCoord coord {};
    std::vector<mi::GigaVoxelVertex> vertices;
    std::vector<uint32_t> indices;
};

// Destroy a chunk's GPU-side mesh data (the per-chunk BLAS + heap range). Sent
// when a chunk is unloaded on the gameplay side. The render-side asset may keep
// the data alive a little longer for fade-out — that's its own lifecycle.
struct DestroyChunkMeshCmd {
    ShellId shell = kInvalidShellId;
    ChunkCoord coord {};
};

using RenderCommand = std::variant<
    UpdateCameraCmd,
    CreateGigaVoxelShellCmd,
    DestroyGigaVoxelShellCmd,
    UploadChunkMeshCmd,
    DestroyChunkMeshCmd>;

// SPSC lock-free ring. gameplay = single producer, render = single consumer.
// Budget 16384 matches the RHI resource-recycle queue (rhi.h) — generous for a
// per-tick command stream at 20 TPS. If Push ever returns false (ring full),
// the gameplay thread should retry next tick rather than drop (commands must
// not be lost: a dropped UploadChunkMesh leaks a chunk visually).
using RenderCommandQueue = mi::TLockFreeQueue<
    RenderCommand, mi::LockFreeQueueUserType::kOne, mi::LockFreeQueueUserType::kOne, 16384>;

MACROMC_NAMESPACE_END

#endif // MACROMC_RENDER_COMMAND_H
