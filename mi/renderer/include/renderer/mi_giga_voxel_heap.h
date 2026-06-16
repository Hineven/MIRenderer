/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_GIGA_VOXEL_HEAP_H
#define MI_GIGA_VOXEL_HEAP_H

#include <cstdint>
#include <span>
#include <vector>

#include <core/base.h>
#include <core/refcounted.h>
#include <core/util/size_class_allocator.h>
#include <rhi/rhi_desc.h>
#include <rhi/rhi_types.h>

#include "../shaders/shared/SharedGigaVoxel.hlsl"

MI_NAMESPACE_BEGIN

class RHICommandQueueGraphics;

// =============================================================================
// GigaVoxelGeometryHeap
// -----------------------------------------------------------------------------
// A paired vertex/index heap for GigaVoxel chunk geometry, built on
// SizeClassAllocator. CPU is the single source of truth for layout: the
// allocator decides every chunk's {vertex_offset, index_offset}, and the GPU
// buffers passively mirror the same offsets (uploaded incrementally).
//
// Design goals (vs. the flat-vector arrangement it replaces):
//   - Allocating / freeing / updating ONE chunk is O(1) (free-list hit) plus an
//     upload of just that chunk's range — no whole-heap rewrite.
//   - CPU and GPU offsets are identical by construction (CPU decides), so no
//     offset translation table is needed.
//   - A Compact() fallback reclaims tail fragmentation when Allocate fails.
//
// BLAS scope is intentionally NOT handled here — the heap only owns the
// vertex/index data. The caller (GigaVoxel) reads Get*HighWatermark() to bound
// its BLAS build.
// =============================================================================

// Identity of one chunk's geometry inside the heap (element units).
struct GigaVoxelChunkHandle {
    uint32_t vertex_offset {};  // element offset into the vertex heap
    uint32_t vertex_count  {};
    uint32_t index_offset  {};  // element offset into the index heap
    uint32_t index_count   {};
    bool     valid         {false};

    FORCEINLINE bool IsEmpty() const { return vertex_count == 0 || index_count == 0; }
};

class GigaVoxelGeometryHeap : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    using VertexT = GigaVoxelVertex;
    using IndexT  = uint32_t;

    // One relocation produced by Compact(): the chunk formerly at old_h now
    // lives at new_h (same counts). The caller must fix up any per-chunk BLAS /
    // handle bookkeeping.
    struct ChunkRelocation {
        GigaVoxelChunkHandle old_h;
        GigaVoxelChunkHandle new_h;
    };

    // `vertex_usage`/`index_usage` are applied to the GPU buffers (include
    // kVertex/kIndex plus whatever else is needed, e.g. AS build input /
    // shader device address). The size classes are tuned for greedy-meshed
    // subchunk geometry.
    GigaVoxelGeometryHeap(RHIBufferUsageFlags vertex_usage,
                          RHIBufferUsageFlags index_usage,
                          size_t initial_vertex_capacity = 1u << 20, // ~1M verts
                          size_t initial_index_capacity  = 1u << 21); // ~2M indices

    // Allocate ranges for a chunk. O(1) on the size-class path. Returns a handle
    // with valid==false if either allocator failed (caller should Compact() or
    // the heap auto-expands on UpdateChunk).
    GigaVoxelChunkHandle AllocateChunk(uint32_t vertex_count, uint32_t index_count);

    // Write a chunk's data into its allocated range (CPU mirror + GPU upload of
    // just that range). If `queue` is null, only the CPU mirror is updated
    // (useful for tests / offline). The handle must come from AllocateChunk and
    // the spans must match its counts.
    void UpdateChunk(const GigaVoxelChunkHandle & handle,
                     std::span<const VertexT> vertices,
                     std::span<const IndexT>  indices,
                     RHICommandQueueGraphics * queue = nullptr);

    // Free a chunk's ranges. CPU recycles immediately; the GPU buffer region
    // becomes reusable for the next allocation (no per-region delayed free —
    // the caller must ensure the GPU is no longer reading this chunk, e.g. via
    // the GigaVoxel frame barrier). O(1).
    void FreeChunk(const GigaVoxelChunkHandle & handle);

    // Defragment both heaps. Relocated chunks are re-uploaded to their new GPU
    // ranges (if queue != null). Returns the list of relocations so the caller
    // can update per-chunk handles / BLAS.
    std::vector<ChunkRelocation> Compact(RHICommandQueueGraphics * queue = nullptr);

    // ---- GPU access (for BLAS build / visibility buffer binding) ----
    RHIBuffer * GetGPUVertexBuffer() const { return gpu_vertex_buffer_.Raw(); }
    RHIBuffer * GetGPUIndexBuffer()  const { return gpu_index_buffer_.Raw(); }
    // Highest occupied offset+1 (element units). Use to bound BLAS builds.
    size_t GetVertexHighWatermark() const { return vertex_allocator_.GetHighWatermark(); }
    size_t GetIndexHighWatermark()  const { return index_allocator_.GetHighWatermark(); }

    // ---- CPU mirror (read-only, for tests / debugging) ----
    std::span<const VertexT> GetCPUVertices() const { return cpu_vertices_; }
    std::span<const IndexT>  GetCPUIndices()  const { return cpu_indices_; }

private:
    void EnsureGPUVertexCapacity(size_t needed_elements, RHICommandQueueGraphics * queue);
    void EnsureGPUIndexCapacity (size_t needed_elements, RHICommandQueueGraphics * queue);

    RHIBufferUsageFlags vertex_usage_;
    RHIBufferUsageFlags index_usage_;

    SizeClassAllocator vertex_allocator_;
    SizeClassAllocator index_allocator_;

    std::vector<VertexT> cpu_vertices_;  // mirror, indexed by allocator offset
    std::vector<IndexT>  cpu_indices_;

    TRef<RHIBuffer> gpu_vertex_buffer_;
    TRef<RHIBuffer> gpu_index_buffer_;
    size_t gpu_vertex_capacity_ {};
    size_t gpu_index_capacity_  {};
};

MI_NAMESPACE_END

#endif // MI_GIGA_VOXEL_HEAP_H
