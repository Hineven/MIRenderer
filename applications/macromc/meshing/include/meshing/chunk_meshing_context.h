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
#include <unordered_map>

#include "meshing/common.h"
#include "meshing/greedy_mesher.h"
#include "meshing/types.h"
#include "core/refcounted.h"
#include "core/task.h"
#include "registry/block_registry.h"
#include "world/chunk_data.h"
#include "world/shell_data.h"
#include "world/types.h"
#include "world/world_data.h"

#include <glm/glm.hpp>

MACROMC_MESHING_NAMESPACE_BEGIN

// Neighbor-aware VoxelSource is implemented in the .cpp as ContextVoxelSource;
// it snapshots neighbor ChunkData via TRef lease (no lock) at construction.

// Per-chunk meshing state tracked by ChunkMeshingContext.
struct ChunkMeshState {
    MACROMC_WORLD_NAMESPACE::ChunkCoord coord;
    std::shared_ptr<VoxelMeshJobResult> result;     // filled by the mesh job
    std::atomic<bool> done {false};
    mi::TaskGroupRef task_group;                    // owns this chunk's mesh tasks (cancel/reprio together)
};

// ChunkMeshingContext: the multi-threaded greedy-meshing scheduler.
//
// Responsibilities:
//   1. Neighbor sharing via TRef lease (NOT a lock): each Entry holds a
//      TRef<ChunkData> for its center chunk. A mesh task snapshots TRef copies
//      of the center + 8 neighbours at scheduling time (sync-gate B has already
//      passed, so the voxel set is stable) and meshes against the snapshot
//      lock-free. This realises TICK_PACING §3.3(II) — zero fine-grained locks
//      on the hot path.
//   2. Distance-based priority: RequestMesh() computes priority from the camera
//      chunk; nearer chunks get higher priority so the world loads near->far.
//   3. Per-chunk TaskGroup: each chunk's mesh job(s) live in a TaskGroup, so a
//      whole chunk's pending work can be cancelled (chunk unloaded) or
//      re-prioritized (camera moved) atomically.
//
// Threading contract (TICK_PACING):
//   - All public mutating methods (RegisterChunk / UnregisterChunk / RequestMesh /
//     UpdatePriorities) and GetResult are called ONLY from the render/main thread,
//     during S2/S3 (after sync-gate B, when the voxel set is stable). The chunks_
//     map is therefore single-threaded and needs no lock.
//   - Mesh task bodies run on worker threads and only touch the TRef<ChunkData>
//     snapshot captured at scheduling time (which extends the chunks' lifetime
//     for the task's duration). Workers never touch chunks_ directly.
//   - RegisterChunk must only be called for chunks whose presence == kReady.
class ChunkMeshingContext : public mi::RefCounted<> {
public:
    static mi::TRef<ChunkMeshingContext> Create(const MACROMC_REGISTRY_NAMESPACE::BlockRegistry& registry);

    // ---- Render/main-thread API (S2/S3, single-threaded; no internal lock) ----

    // Register a chunk as ready for meshing. Must be called after the chunk's
    // data is generated and stable (presence == kReady). Returns false if already
    // registered. The context takes a TRef lease on the chunk, extending its
    // lifetime until UnregisterChunk (and any in-flight mesh of it) completes.
    bool RegisterChunk(MACROMC_WORLD_NAMESPACE::ChunkCoord coord,
                       mi::TRef<MACROMC_WORLD_NAMESPACE::ChunkData> chunk);

    // Remove a chunk (e.g. on unload). Cancels its pending mesh tasks first;
    // any in-flight mesh keeps its own TRef snapshot valid until it finishes.
    void UnregisterChunk(MACROMC_WORLD_NAMESPACE::ChunkCoord coord);

    // Request (re-)meshing of a chunk. Schedules one greedy-mesh task that meshes
    // all subchunks into the chunk's VoxelMeshJobResult. The task captures TRef
    // snapshots of the center + 8 neighbours (those currently registered) so it
    // can read boundary voxels lock-free on the worker.
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

public:
    // Per-chunk registry entry. Public so the worker-side ContextVoxelSource
    // (in the .cpp) can read the map to build its TRef snapshot. The map is only
    // mutated from the render/main thread (see class contract); workers receive
    // TRef copies and never touch the map themselves.
    struct Entry {
        mi::TRef<MACROMC_WORLD_NAMESPACE::ChunkData> chunk;   // TRef lease; keeps chunk alive for meshing
        std::shared_ptr<VoxelMeshJobResult> result;
        std::shared_ptr<std::atomic<bool>> done;               // set by worker; read by GetResult
        mi::TaskRef pending_task;                              // last scheduled mesh task
        mi::TaskGroupRef task_group;
        Entry() : done(std::make_shared<std::atomic<bool>>(false)) {}
        Entry(Entry&& other) noexcept = default;
        Entry& operator=(Entry&& other) noexcept = default;
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
