/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "meshing/chunk_meshing_context.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/task.h"
#include "profiler/profiler.h"
#include "world/chunk_coord.h"

MACROMC_MESHING_NAMESPACE_BEGIN

// ============================================================================
// ContextVoxelSource
//
// Reads neighbor chunks through TRef snapshots captured at task scheduling
// time (no lock). Per TICK_PACING §3.3(II): meshing runs in S2, after sync-gate
// B has passed, so the voxel set is stable — workers need only TRef leases to
// keep the chunks alive for the mesh pass, never a lock.
// ============================================================================
class ContextVoxelSource : public VoxelSource {
public:
    // A snapshot of {coord -> TRef<ChunkData>} for the center + 8 neighbours.
    // Built by RequestMesh on the render/main thread; moved into the task body.
    using Snapshot = std::unordered_map<MACROMC_WORLD_NAMESPACE::ChunkCoord,
                                        mi::TRef<MACROMC_WORLD_NAMESPACE::ChunkData>,
                                        MACROMC_WORLD_NAMESPACE::ChunkCoordHash>;

    explicit ContextVoxelSource(Snapshot snapshot, MACROMC_WORLD_NAMESPACE::ChunkCoord center_coord)
        : center_coord_(center_coord), snapshot_(std::move(snapshot)) {}

    MACROMC_REGISTRY_NAMESPACE::BlockId GetBlockIdAtWorld(int32_t bx, int32_t by, int32_t bz) const override {
        using namespace MACROMC_WORLD_NAMESPACE;
        ChunkCoord cc = BlockWorldToChunkCoord(glm::ivec3(bx, by, bz));
        int32_t lx = bx - cc.x * static_cast<int32_t>(kChunkSizeX);
        int32_t lz = bz - cc.z * static_cast<int32_t>(kChunkSizeZ);

        auto it = snapshot_.find(cc);
        if (it == snapshot_.end() || !it->second) return kAirBlockId;
        if (by < 0 || by >= static_cast<int32_t>(kChunkSizeY)) return kAirBlockId;
        return it->second->GetBlockId(static_cast<uint32_t>(lx), static_cast<uint32_t>(by), static_cast<uint32_t>(lz));
    }

private:
    MACROMC_WORLD_NAMESPACE::ChunkCoord center_coord_;
    Snapshot snapshot_;
};

// ============================================================================
// ChunkMeshingContext
// ============================================================================

ChunkMeshingContext::ChunkMeshingContext(const BlockRegistry& registry)
    : registry_(registry), mesher_(new GreedyMesher(registry)) {}

mi::TRef<ChunkMeshingContext> ChunkMeshingContext::Create(const BlockRegistry& registry) {
    return mi::TRef<ChunkMeshingContext>(new ChunkMeshingContext(registry));
}

uint32_t ChunkMeshingContext::ComputePriority(ChunkCoord a, ChunkCoord b, uint32_t max_distance) {
    // Chebyshev (chessboard) distance in chunk grid; nearer => higher priority.
    int32_t dx = std::abs(a.x - b.x);
    int32_t dz = std::abs(a.z - b.z);
    int32_t dist = std::max(dx, dz);
    if (static_cast<uint32_t>(dist) >= max_distance) return 0;
    return max_distance - static_cast<uint32_t>(dist);
}

bool ChunkMeshingContext::RegisterChunk(ChunkCoord coord, mi::TRef<ChunkData> chunk) {
    // Single-threaded (render/main, S2/S3): no lock. See class contract.
    if (chunks_.find(coord) != chunks_.end()) return false;
    if (!chunk) return false;
    Entry e;
    e.chunk = std::move(chunk);
    e.result = std::make_shared<VoxelMeshJobResult>();
    e.result->Init();
    e.task_group = mi::Create<mi::TaskGroup>();
    chunks_.emplace(coord, std::move(e));
    return true;
}

void ChunkMeshingContext::UnregisterChunk(ChunkCoord coord) {
    // Single-threaded: no lock. Pull the task group out, erase the entry, then
    // cancel outside the (non-existent now) lock region. Cancelling drops the
    // entry's TRef; any in-flight mesh task still holds its own captured TRef
    // snapshot and finishes (or cooperatively no-ops) on its own.
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return;
    mi::TaskGroupRef group = std::move(it->second.task_group);
    it->second.chunk = nullptr;  // release the context's lease
    chunks_.erase(it);
    if (group) group->CancelAll();
}

mi::TaskRef ChunkMeshingContext::RequestMesh(ChunkCoord coord, ChunkCoord camera_chunk,
                                             uint32_t max_priority_distance) {
    using namespace MACROMC_WORLD_NAMESPACE;
    // Single-threaded (render/main, S2/S3): no lock. Snapshot the entry + the
    // neighbour TRefs here, then hand them to the worker task.
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return nullptr;
    Entry& entry = it->second;
    entry.done->store(false);

    uint32_t priority = ComputePriority(coord, camera_chunk, max_priority_distance);

    // Build the TRef snapshot for the center + 8 neighbours (XZ ring; Y is full
    // height). Each TRef copy IncRefs the chunk, keeping it alive for the task.
    ContextVoxelSource::Snapshot snapshot;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz) {
            ChunkCoord nc{coord.x + dx, coord.z + dz};
            auto nit = chunks_.find(nc);
            if (nit != chunks_.end() && nit->second.chunk) {
                snapshot.emplace(nc, nit->second.chunk);  // TRef copy = IncRef
            }
        }

    GreedyMesher* mesher_raw = mesher_.Raw();
    auto result_ptr = entry.result;  // shared_ptr: keep result alive across re-mesh
    auto done_ptr = entry.done;      // shared_ptr<atomic<bool>>: worker signals completion

    // The task body owns the snapshot (TRef leases) and the result; it never
    // touches chunks_ directly, so it is safe to run on a worker lock-free.
    auto body = [coord, mesher_raw, result_ptr, done_ptr, snapshot = std::move(snapshot)]() mutable {
        // Time a single chunk mesh. Null-safe: no-op when no global profiler.
        ::macromc::Profiler::ScopeHandle mesh_scope =
            []() -> ::macromc::Profiler::ScopeHandle {
                auto* p = ::macromc::Profiler::GetGlobal();
                return p ? p->Scope("mesh.single") : ::macromc::Profiler::ScopeHandle{};
            }();
        // The center chunk must be in the snapshot (we registered it).
        auto cit = snapshot.find(coord);
        if (cit == snapshot.end() || !cit->second) return;
        ChunkData* chunk = cit->second.Raw();

        // Construct the neighbour-aware source once (it owns the snapshot),
        // then mesh every non-empty subchunk against it.
        ContextVoxelSource source(std::move(snapshot), coord);

        result_ptr->Clear();
        // NOTE: sy must NOT be uint8_t — kSubChunksPerChunkY==256 would overflow at
        // 255->0 and loop forever. Use a wide int.
        for (uint32_t sy = 0; sy < kSubChunksPerChunkY; ++sy) {
            if (chunk->GetSubChunk(static_cast<uint8_t>(sy)).IsEmpty()) continue;
            glm::ivec3 block_origin(
                coord.x * static_cast<int32_t>(kChunkSizeX),
                static_cast<int32_t>(sy) * static_cast<int32_t>(kSubChunkSize),
                coord.z * static_cast<int32_t>(kSubChunkSize)
            );
            mesher_raw->MeshSubChunk(*chunk, coord, static_cast<uint8_t>(sy), source,
                                     block_origin, result_ptr->subchunk_meshes[sy]);
        }

        // Signal completion so GetResult can return the result next poll.
        done_ptr->store(true, std::memory_order_release);
    };

    auto task = mi::TaskGraph::Get().CreateTask(std::move(body))
                   .SetPriority(static_cast<mi::TaskPriority>(priority))
                   .Done(false);  // do NOT fire yet — BatchEnqueue fires per its contract

    // Attach the task to the chunk's group (single-threaded: no lock).
    it = chunks_.find(coord);
    if (it != chunks_.end()) {
        it->second.pending_task = task;
        if (it->second.task_group) it->second.task_group->Add(task);
    }

    // Fire + enqueue once, via BatchEnqueue (contract: tasks must not be fired yet).
    mi::TaskGraph::Get().BatchEnqueue({task});
    MI_PROF_INCREMENT(mesh_dispatched, 1);  // counter: meshes dispatched this frame
    return task;
}

void ChunkMeshingContext::UpdatePriorities(ChunkCoord camera_chunk, uint32_t max_priority_distance) {
    // Single-threaded: no lock. UpdatePriority itself is thread-safe.
    for (auto& [coord, entry] : chunks_) {
        if (entry.pending_task) {
            uint32_t prio = ComputePriority(coord, camera_chunk, max_priority_distance);
            entry.pending_task->UpdatePriority(static_cast<mi::TaskPriority>(prio));
        }
    }
}

std::shared_ptr<VoxelMeshJobResult> ChunkMeshingContext::GetResult(ChunkCoord coord) const {
    // Single-threaded: no lock.
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return nullptr;
    if (!it->second.done->load(std::memory_order_acquire)) return nullptr;
    return it->second.result;
}

size_t ChunkMeshingContext::GetRegisteredCount() const {
    return chunks_.size();
}

MACROMC_MESHING_NAMESPACE_END
