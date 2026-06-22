/*
 * Created: 2026/06/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "gigavoxel_shell_registry.h"

#include <utility>

#include "renderer/mi_scene.h"

MACROMC_NAMESPACE_BEGIN

GigaVoxelShellRegistry::GigaVoxelShellRegistry(mi::Scene * scene)
    : scene_(scene) {}

GigaVoxelShellRegistry::~GigaVoxelShellRegistry() {
    // Drop every shell's instance. Each instance delayed-frees via the render
    // thread's recycle queue and in turn releases its GigaVoxel asset.
    shells_.clear();
}

void GigaVoxelShellRegistry::HandleCommand(const CreateGigaVoxelShellCmd & cmd) {
    if (cmd.shell == kInvalidShellId) return;
    // Replace an existing shell first (release old instance, then create).
    shells_.erase(cmd.shell);
    shell_indices_.erase(cmd.shell);
    auto gv = mi::GigaVoxel::Create();
    if (!gv) return;
    // Assign a dense shell index (low 8 bits of instance customIndex).
    uint8_t idx;
    if (!shell_index_free_.empty()) {
        idx = shell_index_free_.back();
        shell_index_free_.pop_back();
    } else {
        idx = static_cast<uint8_t>(shell_indices_.size());
    }
    shell_indices_[cmd.shell] = idx;
    gv->SetShellIndex(idx);
    gv->SetPartitionAllocator(&partition_allocator_);
    // Build the instance (owns the asset) and register it into the scene.
    auto instance = mi::GigaVoxelInstance::Create(scene_, gv);
    if (!instance) return;
    shells_[cmd.shell] = std::move(instance);
}

void GigaVoxelShellRegistry::HandleCommand(const DestroyGigaVoxelShellCmd & cmd) {
    shells_.erase(cmd.shell);
    auto it = shell_indices_.find(cmd.shell);
    if (it != shell_indices_.end()) {
        shell_index_free_.push_back(it->second);
        shell_indices_.erase(it);
    }
}

void GigaVoxelShellRegistry::HandleCommand(const UploadChunkMeshCmd & cmd) {
    auto it = shells_.find(cmd.shell);
    if (it == shells_.end()) return;  // shell not created yet / already destroyed
    auto chunk_id = EncodeChunkId(cmd.coord);
    mi::GigaVoxelChunkCoord gv_coord{cmd.coord.x, cmd.coord.z};
    it->second->GetGigaVoxel()->UploadChunk(chunk_id, gv_coord,
                                            std::move(cmd.vertices), std::move(cmd.indices));
}

void GigaVoxelShellRegistry::HandleCommand(const DestroyChunkMeshCmd & cmd) {
    auto it = shells_.find(cmd.shell);
    if (it == shells_.end()) return;
    it->second->GetGigaVoxel()->RemoveChunk(EncodeChunkId(cmd.coord));
}

MACROMC_NAMESPACE_END
