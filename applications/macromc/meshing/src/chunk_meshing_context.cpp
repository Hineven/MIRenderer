/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "meshing/chunk_meshing_context.h"

#include <algorithm>
#include <cmath>

#include "core/task.h"
#include "world/chunk_coord.h"

MACROMC_MESHING_NAMESPACE_BEGIN

// ============================================================================
// NeighborAwareVoxelSource
//
// Reads neighbor chunks through the ChunkMeshingContext's own locked registry,
// which is the "locked global registry sharing edge chunks" the user described.
// The center chunk is read directly (no re-lock); neighbors are read under a
// shared lock held for the lifetime of this source.
// ============================================================================

// ContextVoxelSource: snapshots the center + 8 neighbor ChunkData pointers under
// a SHORT shared lock at construction, then releases the lock and meshes against
// the snapshot. Holding the lock for the entire mesh pass deadlocks (writer
// starvation / WaitForTask interaction); snapshotting avoids that and keeps the
// mesh pass lock-free. The chunk pointers stay valid for the task's lifetime
// because chunks are only unregistered via UnregisterChunk, which cancels the
// owning task group before the chunk memory goes away.
class ContextVoxelSource : public VoxelSource {
public:
    using ChunkMap = ChunkMeshingContext::ChunkMap;

    ContextVoxelSource(const ChunkMap& chunks, std::shared_mutex& mu, ChunkCoord center_coord)
        : center_coord_(center_coord) {
        // Snapshot under a brief shared lock.
        std::shared_lock<std::shared_mutex> lk(mu);
        // Center + horizontal neighbors (chunk grid is XZ; Y is full-height).
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz) {
                ChunkCoord cc{center_coord.x + dx, 0, center_coord.z + dz};
                auto it = chunks.find(cc);
                if (it != chunks.end()) snapshot_[cc] = it->second.chunk;
            }
    }

    BlockId GetBlockIdAtWorld(int32_t bx, int32_t by, int32_t bz) const override {
        ChunkCoord cc = BlockWorldToChunkCoord(glm::ivec3(bx, by, bz));
        int32_t lx = bx - cc.x * static_cast<int32_t>(kChunkSizeX);
        int32_t lz = bz - cc.z * static_cast<int32_t>(kChunkSizeZ);

        auto it = snapshot_.find(cc);
        if (it == snapshot_.end() || it->second == nullptr) return kAirBlockId;
        if (by < 0 || by >= static_cast<int32_t>(kChunkSizeY)) return kAirBlockId;
        return it->second->GetBlockId(static_cast<uint32_t>(lx), static_cast<uint32_t>(by), static_cast<uint32_t>(lz));
    }

private:
    ChunkCoord center_coord_;
    std::unordered_map<ChunkCoord, ChunkData*, ChunkCoordHash> snapshot_;
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

bool ChunkMeshingContext::RegisterChunk(ChunkCoord coord, ChunkData& chunk) {
    std::unique_lock lock(mu_);
    if (chunks_.find(coord) != chunks_.end()) return false;
    Entry e;
    e.chunk = &chunk;
    e.result = std::make_shared<VoxelMeshJobResult>();
    e.result->Init();
    e.task_group = mi::Create<mi::TaskGroup>();
    chunks_.emplace(coord, std::move(e));
    return true;
}

void ChunkMeshingContext::UnregisterChunk(ChunkCoord coord) {
    mi::TaskGroupRef group;
    {
        std::unique_lock lock(mu_);
        auto it = chunks_.find(coord);
        if (it == chunks_.end()) return;
        group = it->second.task_group;
        chunks_.erase(it);
    }
    // Cancel outside the lock to avoid blocking workers on teardown.
    if (group) group->CancelAll();
}

mi::TaskRef ChunkMeshingContext::RequestMesh(ChunkCoord coord, ChunkCoord camera_chunk,
                                             uint32_t max_priority_distance) {
    // Snapshot what we need under the lock, then schedule the task.
    Entry* entry_ptr = nullptr;
    uint32_t priority = ComputePriority(coord, camera_chunk, max_priority_distance);
    {
        std::unique_lock lock(mu_);
        auto it = chunks_.find(coord);
        if (it == chunks_.end()) return nullptr;
        entry_ptr = &it->second;
        entry_ptr->done.store(false);
    }

    // Build a source. The task body will hold the shared lock via the source
    // for the duration of meshing; RegisterChunk/Unregister take exclusive lock,
    // so structural changes are blocked while a mesh is in flight.
    ChunkCoord coord_copy = coord;
    ChunkMap* chunks_raw = &chunks_;
    std::shared_mutex* mu_raw = &mu_;
    GreedyMesher* mesher_raw = mesher_.Raw();
    auto result_ptr = entry_ptr->result; // shared_ptr: keep result alive

    // Capture raw pointers; the context outlives its tasks (tasks cancelled on
    // context destruction is the caller's responsibility via UnregisterChunk).
    auto body = [coord_copy, chunks_raw, mu_raw, mesher_raw, result_ptr]() {
        // Source snapshots the center + neighbors under a brief shared lock,
        // then meshes lock-free. The chunk snapshot stays valid for this task's
        // lifetime (UnregisterChunk cancels the task group before freeing chunks).
        ContextVoxelSource source(*chunks_raw, *mu_raw, coord_copy);
        auto it = chunks_raw->find(coord_copy);
        if (it == chunks_raw->end()) return; // unregistered concurrently
        ChunkData* chunk = it->second.chunk;
        if (!chunk) return;

        result_ptr->Clear();
        // NOTE: sy must NOT be uint8_t — kSubChunksPerChunkY==256 would overflow at
        // 255->0 and loop forever. Use a wide int.
        for (uint32_t sy = 0; sy < kSubChunksPerChunkY; ++sy) {
            if (chunk->GetSubChunk(static_cast<uint8_t>(sy)).IsEmpty()) continue;
            glm::ivec3 block_origin(
                coord_copy.x * static_cast<int32_t>(kChunkSizeX),
                static_cast<int32_t>(sy) * static_cast<int32_t>(kSubChunkSize),
                coord_copy.z * static_cast<int32_t>(kChunkSizeZ)
            );
            mesher_raw->MeshSubChunk(*chunk, coord_copy, static_cast<uint8_t>(sy), source, block_origin, result_ptr->subchunk_meshes[sy]);
        }

        if (it != chunks_raw->end()) it->second.done.store(true);
    };

    auto task = mi::TaskGraph::Get().CreateTask(std::move(body))
                   .SetPriority(static_cast<mi::TaskPriority>(priority)).Done();

    {
        std::unique_lock lock(mu_);
        auto it = chunks_.find(coord);
        if (it != chunks_.end()) {
            it->second.pending_task = task;
            if (it->second.task_group) it->second.task_group->Add(task);
        }
    }

    // Fire the task.
    mi::TaskGraph::Get().BatchEnqueue({task});
    return task;
}

void ChunkMeshingContext::UpdatePriorities(ChunkCoord camera_chunk, uint32_t max_priority_distance) {
    std::vector<std::pair<ChunkCoord, mi::TaskRef>> to_update;
    {
        std::shared_lock lock(mu_);
        for (auto& [coord, entry] : chunks_) {
            if (entry.pending_task) to_update.emplace_back(coord, entry.pending_task);
        }
    }
    for (auto& [coord, task] : to_update) {
        uint32_t prio = ComputePriority(coord, camera_chunk, max_priority_distance);
        task->UpdatePriority(static_cast<mi::TaskPriority>(prio));
    }
}

std::shared_ptr<VoxelMeshJobResult> ChunkMeshingContext::GetResult(ChunkCoord coord) const {
    std::shared_lock lock(mu_);
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return nullptr;
    if (!it->second.done.load()) return nullptr;
    return it->second.result;
}

size_t ChunkMeshingContext::GetRegisteredCount() const {
    std::shared_lock lock(mu_);
    return chunks_.size();
}

MACROMC_MESHING_NAMESPACE_END
