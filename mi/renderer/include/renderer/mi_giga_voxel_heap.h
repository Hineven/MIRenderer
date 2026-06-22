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

#include <glm/glm.hpp>

#include <core/base.h>
#include <core/refcounted.h>
#include <core/util/size_class_allocator.h>
#include <core/util/slot_allocator.h>
#include <renderer/mi_aabb.h>
#include <rhi/rhi_desc.h>
#include <rhi/rhi_types.h>

#include "../shaders/shared/SharedGigaVoxel.hlsl"

MI_NAMESPACE_BEGIN

class RHICommandQueueGraphics;

// =============================================================================
// Chunk layout convention (renderer-layer knowledge).
//
// A GigaVoxel chunk is a full-height column of voxels, laid out as a 2D grid in
// the XZ plane (one chunk per (x,z) coord), full height in Y. The renderer needs
// these dimensions to turn a chunk coordinate into a world placement, because
// chunk vertices are stored in CHUNK-LOCAL space (local origin = the chunk's
// world corner) and the per-chunk world placement is reconstructed from the
// coordinate here rather than baked into the vertices.
//
// These match the macromc chunk dimensions (16x16 footprint, 4096 tall). They
// are renderer-layer compile-time constants so the renderer does NOT depend on
// macromc; the producer (viewer / macromc_app) passes a GigaVoxelChunkCoord and
// the renderer derives the world origin.
//
// Defined in this header (not mi_giga_voxel.h) because GigaVoxelChunkHandle
// below embeds GigaVoxelChunkCoord, and mi_giga_voxel.h includes this file.
// =============================================================================
inline static constexpr uint32_t kGigaVoxelChunkSizeX = 16;   // chunk footprint in X (meters)
inline static constexpr uint32_t kGigaVoxelChunkSizeY = 4096; // chunk height   in Y (meters)
inline static constexpr uint32_t kGigaVoxelChunkSizeZ = 16;   // chunk footprint in Z (meters)

// 2D chunk coordinate on the XZ tiling grid. This is the renderer-layer mirror
// of macromc's ChunkCoord (kept separate to avoid mi/ depending on macromc).
// The producer converts its own coord type to this at the UploadChunk call site.
struct GigaVoxelChunkCoord {
    int32_t x {};
    int32_t z {};
};

// World-space origin (min corner) of a chunk column. Vertices in that chunk are
// local to this origin, so world = ChunkWorldOrigin(coord) + local_position.
// Y is always 0: chunks are full-height columns, not split along Y, so the
// chunk-local Y axis already covers the full 0..kGigaVoxelChunkSizeY range.
FORCEINLINE glm::vec3 GigaVoxelChunkWorldOrigin(GigaVoxelChunkCoord coord) {
    return glm::vec3(
        static_cast<float>(coord.x * static_cast<int32_t>(kGigaVoxelChunkSizeX)),
        0.0f,
        static_cast<float>(coord.z * static_cast<int32_t>(kGigaVoxelChunkSizeZ))
    );
}

// Chunk-local AABB for a full chunk column (the maximal extent a chunk-local
// vertex can reach). Used as the per-chunk local AABB when the geometry does
// not need an exact vertex scan.
FORCEINLINE AABB GigaVoxelChunkLocalAABB() {
    return AABB(
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(static_cast<float>(kGigaVoxelChunkSizeX),
                  static_cast<float>(kGigaVoxelChunkSizeY),
                  static_cast<float>(kGigaVoxelChunkSizeZ))
    );
}

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
// `chunk_header_index` is a stable global index (0..N) into the per-chunk
// header buffer (GigaVoxelChunkHeaderBuffer). It is assigned once at
// AllocateChunk and freed at FreeChunk, and lets a visibility-buffer pixel /
// RT hit recover this chunk's vertex/index span from the GPU side.
// `coord` is the chunk's 2D grid coordinate; the heap writes the derived world
// origin (GigaVoxelChunkWorldOrigin) into the chunk header so the raster /
// visibility path can place chunk-local vertices into world space.
struct GigaVoxelChunkHandle {
    uint32_t vertex_offset {};  // element offset into the vertex heap
    uint32_t vertex_count  {};
    uint32_t index_offset  {};  // element offset into the index heap
    uint32_t index_count   {};
    uint32_t chunk_header_index {0xFFFFFFFFu};  // index into GigaVoxelChunkHeaderBuffer
    GigaVoxelChunkCoord coord {};               // 2D grid coord -> world origin
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
    // Per-chunk geometry header buffer (GigaVoxelChunkHeader rows, indexed by
    // GigaVoxelChunkHandle::chunk_header_index). Bound by the renderer as
    // GigaVoxelChunkHeaderBuffer SRV for visibility-buffer decode + RT hits.
    RHIBuffer * GetGPUChunkHeaderBuffer() const { return gpu_chunk_header_buffer_.Raw(); }
    // Highest occupied offset+1 (element units). Use to bound BLAS builds.
    size_t GetVertexHighWatermark() const { return vertex_allocator_.GetHighWatermark(); }
    size_t GetIndexHighWatermark()  const { return index_allocator_.GetHighWatermark(); }

    // ---- Per-chunk header update ----
    // Write one chunk's header row (CPU mirror + GPU upload of just that row).
    // No-op if chunk_header_index is invalid. Called by GigaVoxel after a
    // chunk's geometry is uploaded/updated.
    void UpdateChunkHeader(const GigaVoxelChunkHandle & handle, RHICommandQueueGraphics * queue = nullptr);

    // ---- CPU mirror (read-only, for tests / debugging) ----
    std::span<const VertexT> GetCPUVertices() const { return cpu_vertices_; }
    std::span<const IndexT>  GetCPUIndices()  const { return cpu_indices_; }

private:
    void EnsureGPUVertexCapacity(size_t needed_elements, RHICommandQueueGraphics * queue);
    void EnsureGPUIndexCapacity (size_t needed_elements, RHICommandQueueGraphics * queue);
    void EnsureGPUChunkHeaderCapacity(size_t needed_rows, RHICommandQueueGraphics * queue);

    // Allocate / free a global chunk-header index (slot into the per-chunk
    // header buffer). Thin wrappers over chunk_header_slot_allocator_ that also
    // keep cpu_chunk_headers_ sized to the issued slot and clear a row on free.
    uint32_t AllocateChunkHeaderSlot();
    void FreeChunkHeaderSlot(uint32_t slot);

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

    // Per-chunk geometry header buffer (GigaVoxelChunkHeader rows).
    // cpu_chunk_headers_ is indexed by chunk_header_index; the GPU buffer is a
    // 1:1 mirror. Capacity grows on demand (see EnsureGPUChunkHeaderCapacity).
    // The slot allocator is the sole authority on chunk_header_index values --
    // it is deliberately decoupled from cpu_chunk_headers_.size() so that a
    // pre-sized CPU mirror does not perturb slot numbering (a previous hand-
    // rolled free-list conflated the two and made the first real chunk land at
    // index 4096, past the 12-bit RT custom-index ceiling).
    std::vector<GigaVoxelChunkHeader> cpu_chunk_headers_;
    TRef<RHIBuffer> gpu_chunk_header_buffer_;
    size_t gpu_chunk_header_capacity_ {};
    // Allocates / recycles chunk-header slot indices independently of the CPU
    // mirror's size. AllocateSlot() reuses freed slots first, else issues the
    // next monotonic index.
    ExtendableSlotAllocator chunk_header_slot_allocator_;
};

MI_NAMESPACE_END

#endif // MI_GIGA_VOXEL_HEAP_H
