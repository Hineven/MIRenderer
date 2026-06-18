/*
 * Created: 2026/06/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_PARTITION_ALLOCATOR_H
#define MI_PARTITION_ALLOCATOR_H

#include <cstdint>
#include <unordered_set>

#include <core/base.h>

MI_NAMESPACE_BEGIN

// =============================================================================
// PartitionAllocator — renderer-layer allocator for PTLAS partition indices.
//
// PTLAS (VK_NV_partitioned_acceleration_structure) partitions are a GLOBAL
// resource: every non-global partition index must be unique across all
// renderables in the scene. This allocator hands out / reclaims partition ids
// so that renderables (e.g. GigaVoxel, one partition per chunk) do not collide.
//
// Current phase (legacy TLAS): the allocator is BOOKKEEPING-ONLY — it assigns
// stable ids and tracks the dirty set, but the legacy TLAS build ignores
// partition ids (it rebuilds all instances every frame). The partial-rebuild
// benefit materializes once PTLAS RHI support lands.
//
// Threading: render-thread-only (called from the TLAS gather / renderable
// Update path).
// =============================================================================
class PartitionAllocator : public NonCopyable, public NonMovable {
public:
    PartitionAllocator() = default;
    ~PartitionAllocator() = default;

    // Allocate a fresh partition id. Returns a stable id until FreePartition.
    // ids are monotonically increasing; freed ids are NOT recycled yet (the
    // pool is large enough for VC-scale streaming; recycling can be added when
    // TFC/LFC pressure appears).
    uint32_t AllocatePartition() {
        uint32_t id = next_id_++;
        live_.insert(id);
        dirty_.insert(id);
        return id;
    }

    // Mark a partition as dirty (needs PTLAS partial rebuild this frame).
    void MarkDirty(uint32_t id) { dirty_.insert(id); }

    // Release a partition id. It becomes invalid for new instances; the caller
    // must have already removed its instances from the gather.
    void FreePartition(uint32_t id) {
        live_.erase(id);
        dirty_.erase(id);
    }

    // Consume + clear the dirty set (the renderer rebuilds these partitions).
    std::unordered_set<uint32_t> ConsumeDirty() {
        std::unordered_set<uint32_t> out;
        out.swap(dirty_);
        return out;
    }

    bool IsLive(uint32_t id) const { return live_.count(id) > 0; }
    size_t GetLiveCount() const { return live_.size(); }

private:
    uint32_t next_id_ {1};  // 0 reserved for global partition convention
    std::unordered_set<uint32_t> live_;
    std::unordered_set<uint32_t> dirty_;
};

MI_NAMESPACE_END

#endif // MI_PARTITION_ALLOCATOR_H
