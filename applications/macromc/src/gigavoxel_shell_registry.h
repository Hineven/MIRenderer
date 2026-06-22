/*
 * Created: 2026/06/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_GIGAVOXEL_SHELL_REGISTRY_H
#define MACROMC_GIGAVOXEL_SHELL_REGISTRY_H

#include <unordered_map>

#include "core/base.h"
#include "core/refcounted.h"
#include "renderer/mi_giga_voxel.h"
#include "world/types.h"

#include "render_command.h"

MACROMC_NAMESPACE_BEGIN

// =============================================================================
// GigaVoxelShellRegistry — render-thread-side owner of GigaVoxel shells.
//
// Maps each ShellId to its GigaVoxelInstance (one instance per shell; the shell
// is the data source, the instance is the render projection). Lives INSIDE the
// RenderThreadContext and is driven by RenderCommand dispatch — it never sees
// the gameplay thread.
//
// Ownership: the registry owns the TRef<GigaVoxelInstance> per shell. Each
// instance owns its TRef<GigaVoxel> asset (mirrors StaticMesh/StaticMeshInstance:
// the asset knows nothing about its instance). Destroying a shell drops the
// instance, which delayed-frees via the render thread's recycle queue and in
// turn releases the asset.
//
// Threading: all methods are render-thread-only (called from
// RenderThreadContext::ApplyCommand / the render loop).
// =============================================================================
class GigaVoxelShellRegistry : public mi::NonCopyable, public mi::NonMovable {
public:
    explicit GigaVoxelShellRegistry(mi::Scene * scene);
    ~GigaVoxelShellRegistry();

    // The partition allocator shared by all shells' GigaVoxel assets. Each
    // chunk gets one partition id from this allocator. Bookkeeping-only until
    // PTLAS RHI support lands (legacy TLAS ignores partition ids).
    mi::PartitionAllocator & PartitionAlloc() { return partition_allocator_; }

    // Command handlers (one per shell-level RenderCommand). Idempotent where it
    // makes sense: Create on an existing shell replaces it; Destroy on an
    // unknown shell is a no-op.
    void HandleCommand(const CreateGigaVoxelShellCmd & cmd);
    void HandleCommand(const DestroyGigaVoxelShellCmd & cmd);
    void HandleCommand(const UploadChunkMeshCmd & cmd);
    void HandleCommand(const DestroyChunkMeshCmd & cmd);

    bool Has(ShellId shell) const { return shells_.count(shell) > 0; }
    size_t GetShellCount() const { return shells_.size(); }

private:
    // Encode a 2D ChunkCoord into the GigaVoxelChunkId space (stable, injective).
    // 21 bits per axis covers ±1M chunks — far beyond any realistic world.
    static mi::GigaVoxelChunkId EncodeChunkId(const ChunkCoord & c) {
        return (static_cast<mi::GigaVoxelChunkId>(static_cast<uint32_t>(c.x)) << 21)
             |  static_cast<mi::GigaVoxelChunkId>(static_cast<uint32_t>(c.z));
    }

    mi::Scene * scene_ = nullptr;
    std::unordered_map<ShellId, mi::TRef<mi::GigaVoxelInstance>> shells_;
    // Dense shell index allocator (0..255, the low 8 bits of instance customIndex).
    // One per shell that has a GigaVoxel asset. Recycled on shell destroy.
    // Stored alongside the asset so HandleCommand(Create/Destroy) manages it.
    std::unordered_map<ShellId, uint8_t> shell_indices_;
    std::vector<uint8_t> shell_index_free_;
    mi::PartitionAllocator partition_allocator_;
};

MACROMC_NAMESPACE_END

#endif // MACROMC_GIGAVOXEL_SHELL_REGISTRY_H
