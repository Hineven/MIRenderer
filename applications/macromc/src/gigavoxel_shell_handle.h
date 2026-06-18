/*
 * Created: 2026/06/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_GIGAVOXEL_SHELL_HANDLE_H
#define MACROMC_GIGAVOXEL_SHELL_HANDLE_H

#include <unordered_set>
#include <vector>

#include "renderer/mi_giga_voxel.h"
#include "world/types.h"

#include "render_command.h"

MACROMC_NAMESPACE_BEGIN

// =============================================================================
// GigaVoxelShellHandle — game-thread-side logical handle for one shell's
// GigaVoxel render projection.
//
// The gameplay thread holds one of these per shell it wants rendered. The
// handle is PURE LOGICAL STATE: it holds no RHI resources, no GigaVoxel asset.
// Every operation is translated into a RenderCommand pushed into the
// RenderCommandQueue (the SPSC bridge to the render thread, where the real
// GigaVoxelShellRegistry owns the asset).
//
// It tracks which chunk coords it has uploaded so it can batch-destroy them
// when the shell goes away (ShellId tags every command).
//
// Threading: gameplay-thread-only. The queue pointer is set at construction and
// must outlive the handle.
// =============================================================================
class GigaVoxelShellHandle {
public:
    GigaVoxelShellHandle(ShellId shell, RenderCommandQueue * queue)
        : shell_(shell), queue_(queue) {}

    // Request creation of the render-side GigaVoxel asset for this shell.
    void Create(ShellCategory category) {
        if (!queue_) return;
        CreateGigaVoxelShellCmd cmd;
        cmd.shell = shell_;
        cmd.category = category;
        Push(std::move(cmd));
    }

    // Upload/refresh one chunk's mesh (flattened + converted on the render
    // thread in the current architecture, since meshing results are only
    // readable there). Indices must be chunk-local (0-based).
    void UploadChunk(ChunkCoord coord,
                     std::vector<mi::GigaVoxelVertex> vertices,
                     std::vector<uint32_t> indices) {
        if (!queue_) return;
        UploadChunkMeshCmd cmd;
        cmd.shell = shell_;
        cmd.coord = coord;
        cmd.vertices = std::move(vertices);
        cmd.indices = std::move(indices);
        uploaded_.insert(coord);
        Push(std::move(cmd));
    }

    // Destroy one chunk's GPU mesh.
    void DestroyChunk(ChunkCoord coord) {
        if (!queue_) return;
        DestroyChunkMeshCmd cmd;
        cmd.shell = shell_;
        cmd.coord = coord;
        uploaded_.erase(coord);
        Push(std::move(cmd));
    }

    // Tear down the whole shell: destroy every uploaded chunk, then the asset.
    void Destroy() {
        if (!queue_) return;
        for (const auto & coord : uploaded_) {
            DestroyChunkMeshCmd cmd;
            cmd.shell = shell_;
            cmd.coord = coord;
            Push(std::move(cmd));
        }
        uploaded_.clear();
        DestroyGigaVoxelShellCmd cmd;
        cmd.shell = shell_;
        Push(std::move(cmd));
    }

    ShellId GetShellId() const { return shell_; }

private:
    void Push(RenderCommand cmd) {
        // In bypass mode EnqueueCommand runs inline; otherwise it Pushes to the
        // SPSC ring. The queue itself is owned by RenderThreadContext.
        queue_->Push(std::move(cmd));
    }

    ShellId shell_ = kInvalidShellId;
    RenderCommandQueue * queue_ = nullptr;
    std::unordered_set<ChunkCoord, ChunkCoordHash> uploaded_;
};

MACROMC_NAMESPACE_END

#endif // MACROMC_GIGAVOXEL_SHELL_HANDLE_H
