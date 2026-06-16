/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_MESHING_CHUNK_MESHING_CONTEXT_H
#define MACROMC_MESHING_CHUNK_MESHING_CONTEXT_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "meshing/common.h"
#include "meshing/greedy_mesher.h"
#include "meshing/types.h"
#include "core/task.h"
#include "registry/block_registry.h"
#include "world/shell_data.h"
#include "world/types.h"
#include "world/world_data.h"

#include <glm/glm.hpp>

MACROMC_MESHING_NAMESPACE_BEGIN

// Neighbor-aware VoxelSource is implemented in the .cpp as ContextVoxelSource;
// it reads neighbor chunks through the context's own locked registry.

// Per-chunk meshing state tracked by ChunkMeshingContext.
struct ChunkMeshState {
    MACROMC_WORLD_NAMESPACE::ChunkCoord coord;
    std::shared_ptr<VoxelMeshJobResult> result;     // filled by the mesh job
    std::atomic<bool> done {false};
    mi::TaskGroupRef task_group;                    // owns this chunk's mesh tasks (cancel/reprio together)
};

// ChunkMeshingContext: the multi-threaded greedy-meshing scheduler.
//
// Responsibilities (matches the user's design):
//   1. Thread-safe neighbor sharing: wraps a WorldShellData with a shared_mutex
//      so worker threads can read neighbor chunk boundary voxels safely, while
//      the main/render thread can register/unregister chunks under exclusive lock.
//   2. Distance-based priority: RequestMesh() computes priority from the camera
//      chunk; nearer chunks get higher priority so the world loads near->far.
//   3. Per-chunk TaskGroup: each chunk's mesh job(s) live in a TaskGroup, so a
//      whole chunk's pending work can be cancelled (chunk unloaded) or
//      re-prioritized (camera moved) atomically.
//
// Threading: RequestMesh/cancel/result access is intended to be driven from the
// render/main thread; the mesh task bodies run on worker threads and only touch
// chunk data that was already registered (kGenerated state) plus neighbor reads
// via the locked source.
class ChunkMeshingContext : public mi::RefCounted<> {
public:
    static mi::TRef<ChunkMeshingContext> Create(const MACROMC_REGISTRY_NAMESPACE::BlockRegistry& registry);

    // ---- Main/render-thread API (mutating, exclusive lock) ----

    // Register a chunk as ready for meshing. Must be called after the chunk's
    // data is generated and stable. Returns false if already registered.
    bool RegisterChunk(MACROMC_WORLD_NAMESPACE::ChunkCoord coord,
                       MACROMC_WORLD_NAMESPACE::ChunkData& chunk);

    // Remove a chunk (e.g. on unload). Cancels its pending mesh tasks.
    void UnregisterChunk(MACROMC_WORLD_NAMESPACE::ChunkCoord coord);

    // Request (re-)meshing of a chunk. Schedules one greedy-mesh task that meshes
    // all subchunks into the chunk's VoxelMeshJobResult.
    //   camera_chunk : the chunk the camera is in, for distance-based priority.
    //   max_priority_distance : chunks farther than this get priority 0.
    // Returns the task (so the caller can wait/cancel). Nullptr if the
    // chunk isn't registered.
    mi::TaskRef RequestMesh(MACROMC_WORLD_NAMESPACE::ChunkCoord coord,
                            MACROMC_WORLD_NAMESPACE::ChunkCoord camera_chunk,
                            uint32_t max_priority_distance = 128);

    // Re-prioritize all pending chunk meshes against a new camera position.
    void UpdatePriorities(MACROMC_WORLD_NAMESPACE::ChunkCoord camera_chunk,
                          uint32_t max_priority_distance = 128);

    // ---- Worker-thread API (read-only, shared lock) ----

    // Take a mesh result if the chunk has finished meshing. Returns nullptr otherwise.
    // The returned shared_ptr keeps the result alive independent of later re-mesh.
    std::shared_ptr<VoxelMeshJobResult> GetResult(MACROMC_WORLD_NAMESPACE::ChunkCoord coord) const;

    // ---- Accessors ----
    size_t GetRegisteredCount() const;
    const GreedyMesher& GetMesher() const { return *mesher_; }

private:
    explicit ChunkMeshingContext(const MACROMC_REGISTRY_NAMESPACE::BlockRegistry& registry);

    // Distance-based priority: nearer => higher priority. Clamped to [0, max].
    static uint32_t ComputePriority(MACROMC_WORLD_NAMESPACE::ChunkCoord a,
                                    MACROMC_WORLD_NAMESPACE::ChunkCoord b,
                                    uint32_t max_distance);

    const MACROMC_REGISTRY_NAMESPACE::BlockRegistry& registry_;
    mi::TRef<GreedyMesher> mesher_;

    // Protects chunks_ map (structural mutations) AND provides shared reads for
    // neighbor lookups during meshing. A single coarse lock over the whole set;
    // acceptable for VC-scale streaming (hundreds of chunks).
    mutable std::shared_mutex mu_;

public:
    // Per-chunk registry entry. Public so the worker-side VoxelSource (in the
    // .cpp) can read the map under the shared lock.
    struct Entry {
        MACROMC_WORLD_NAMESPACE::ChunkData* chunk = nullptr;   // non-owning; owned by the shell
        std::shared_ptr<VoxelMeshJobResult> result;
        std::atomic<bool> done {false};
        mi::TaskRef pending_task;                              // last scheduled mesh task
        mi::TaskGroupRef task_group;
        // Constructor needed because Entry holds an atomic (non-movable by default).
        Entry() = default;
        Entry(Entry&& other) noexcept
            : chunk(other.chunk), result(std::move(other.result)),
              done(other.done.load()), pending_task(std::move(other.pending_task)),
              task_group(std::move(other.task_group)) {}
        Entry& operator=(Entry&& other) noexcept {
            if (this != &other) {
                chunk = other.chunk;
                result = std::move(other.result);
                done.store(other.done.load());
                pending_task = std::move(other.pending_task);
                task_group = std::move(other.task_group);
            }
            return *this;
        }
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
    };
    using ChunkMap = std::unordered_map<MACROMC_WORLD_NAMESPACE::ChunkCoord, Entry,
                                        MACROMC_WORLD_NAMESPACE::ChunkCoordHash>;

private:
    ChunkMap chunks_;
};

MACROMC_MESHING_NAMESPACE_END

#endif // MACROMC_MESHING_CHUNK_MESHING_CONTEXT_H
