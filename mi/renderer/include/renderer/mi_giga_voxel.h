/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_GIGA_VOXEL_H
#define MI_GIGA_VOXEL_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <core/base.h>
#include <core/refcounted.h>
#include <renderer/mi_aabb.h>
#include <renderer/mi_dirty_tracker.h>
#include <renderer/mi_giga_voxel_heap.h>
#include <renderer/mi_renderable.h>
#include <renderer/mi_renderer_fwd.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_texture.h>
#include <rdg/rdg_ray_tracing_registry.h>

#include "../shaders/shared/SharedGigaVoxel.hlsl"

MI_NAMESPACE_BEGIN

// =============================================================================
// GigaVoxel device-side data layer (Phase 1: VC skeleton).
//
// A GigaVoxel asset is the GPU counterpart of a voxel terrain (macromc world).
// It occupies ONE bindless slot (SlotKind::GigaVoxel) and, per the visibility
// buffer design, a single GigaVoxelInstance occupies ONE RenderableIndex slot —
// internal chunk streaming does NOT register thousands of renderables with the
// Scene.
//
// Phase 1 scope (this file):
//   - Vertex/index geometry is uploaded into the GigaVoxel-dedicated vertex/
//     index uber buffers in DeviceBindlessResourceAllocator (kept separate from
//     the StaticMesh geometry heaps, which have a load-once access pattern).
//   - A single BLAS is built over the merged uploaded geometry. This is a
//     SKELETON / bring-up arrangement: it merges all chunk geometry into one
//     vertex/index range and rebuilds one BLAS on every UpdateOnDevice_Async.
//     It is NOT a scalable solution for real chunk counts — the real design
//     (per-chunk BLAS + streaming + TLAS/PTLAS) lands in later phases.
//   - A block-texture atlas (4096^2, 256x256 tiles of 16^2) is referenced by
//     bindless index through GigaVoxelHeader.AtlasBindlessIndex.
// =============================================================================

// -----------------------------------------------------------------------------
// DeviceGigaVoxel
// -----------------------------------------------------------------------------
// Owns the GPU handles for one GigaVoxel asset: bindless slot and the (Phase-1:
// single) BLAS. Vertex/index geometry lives in the GigaVoxelGeometryHeap owned
// by the GigaVoxel asset (shared across all chunks), not here.
class DeviceGigaVoxel : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    FORCEINLINE bool IsValid() const { return slot_ && slot_->Get() != UINT32_MAX; }
    FORCEINLINE uint32_t GetIndex() const { return slot_ ? slot_->Get() : UINT32_MAX; }
    FORCEINLINE RHIAccelerationStructure * GetBLAS() const { return BLAS_.Raw(); }

protected:
    DeviceGigaVoxel(DeviceBindlessResourceAllocator * allocator);
    ~DeviceGigaVoxel() override;

    // Bindless slot keeper (delayed free through the allocator).
    TRef<DeviceBindlessResourceAllocator::SlotKeeper> slot_;

    // Phase-1 single BLAS over the merged geometry.
    TRef<RHIAccelerationStructure> BLAS_;

    friend class GigaVoxel;
};

// -----------------------------------------------------------------------------
// GigaVoxel (Asset)
// -----------------------------------------------------------------------------
// Host-side voxel terrain asset, parallel to StaticMesh / VolumeGrid. Accepts
// chunk geometry (already converted to the renderer-layer GigaVoxelVertex) and
// an atlas texture, then uploads + builds device resources on demand.
//
// Chunk geometry is managed by a GigaVoxelGeometryHeap (size-class allocator):
// each chunk gets a stable {vertex_offset, index_offset} and can be added /
// removed / updated independently in O(1) (plus an incremental GPU upload of
// just that chunk's range). CPU is the source of truth for offsets; the GPU
// buffers mirror them.
//
// ChunkId identifies a chunk; the caller picks the id space (e.g. a packed
// chunk coord or a monotonic counter). One id maps to at most one live handle.

// Identifier for a chunk inside a GigaVoxel asset. Opaque 64-bit; the caller
// defines the encoding.
using GigaVoxelChunkId = uint64_t;
static constexpr GigaVoxelChunkId kInvalidGigaVoxelChunkId = UINT64_MAX;

class GigaVoxel : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    friend class Renderer;

    static TRef<GigaVoxel> Create();

    // ---- Atlas (block texture atlas, 4096^2, 256x256 tiles of 16^2) ----
    // The texture must already be converted to bindless; its bindless index is
    // recorded into GigaVoxelHeader.AtlasBindlessIndex.
    void SetAtlasTexture(TRef<Texture> atlas);
    FORCEINLINE Texture * GetAtlasTexture() const { return atlas_.Raw(); }

    // ---- Chunk geometry (heap-backed; O(1) alloc/free + incremental upload) ----
    // Add a new chunk. Allocates ranges, uploads to GPU immediately (the queue
    // from UpdateOnDevice is NOT required — a fresh upload uses the global
    // graphics queue), marks dirty. Returns the handle; valid==false on
    // allocation failure (caller may CompactChunks() and retry). If `id` is
    // already live, it is replaced (old range freed first).
    GigaVoxelChunkHandle UploadChunk(GigaVoxelChunkId id,
                                     std::vector<GigaVoxelVertex> vertices,
                                     std::vector<uint32_t> indices);
    // Replace an existing chunk's geometry (free old range, allocate new,
    // upload). If `id` is unknown this is equivalent to UploadChunk.
    void UpdateChunk(GigaVoxelChunkId id,
                     std::vector<GigaVoxelVertex> vertices,
                     std::vector<uint32_t> indices);
    // Remove a chunk and free its ranges. No-op if unknown.
    void RemoveChunk(GigaVoxelChunkId id);
    // Remove all chunks.
    void ClearAllChunks();
    // Defragment the geometry heap (low-frequency fallback). Re-uploads moved
    // chunks. The chunk id->handle map is updated in place.
    void CompactChunks();

    FORCEINLINE DeviceGigaVoxel * GetDeviceGigaVoxel() const { return device_giga_voxel_.Raw(); }
    FORCEINLINE GigaVoxelGeometryHeap * GetGeometryHeap() const { return geometry_heap_.Raw(); }
    FORCEINLINE bool IsEmpty() const { return chunk_handles_.empty(); }
    FORCEINLINE AABB GetAABB() const { return aabb_; }

    // Update device resources. Recording commands go to the given graphics
    // queue; caller is responsible for submission + synchronization. In the
    // heap-backed design, per-chunk uploads already happen on Upload/Remove;
    // this call rebuilds the (Phase-1 single) BLAS over the live heap range
    // and refreshes the GigaVoxelHeader.
    void UpdateOnDevice_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue);
    // Convenience sync wrapper that waits idle.
    void UpdateOnDevice(DeviceBindlessResourceAllocator * alloc);

    FORCEINLINE bool IsDirty() const { return dirty_; }
    void SetDirty(bool dirty = true);

    FORCEINLINE bool IsRayTraced() const { return ray_traced_; }
    FORCEINLINE void SetRayTraced(bool ray_traced) {
        if (ray_traced != ray_traced_) {
            ray_traced_ = ray_traced;
            SetDirty();
        }
    }

protected:
    GigaVoxel();

    TRef<DeviceGigaVoxel> device_giga_voxel_;
    TRef<Texture> atlas_;
    TRef<GigaVoxelGeometryHeap> geometry_heap_;

    // chunk id -> live handle. Drives the heap and the BLAS range.
    std::unordered_map<GigaVoxelChunkId, GigaVoxelChunkHandle> chunk_handles_;
    uint32_t atlas_bindless_index_ {0xFFFFFFFFu};

    AABB aabb_ {};
    bool dirty_ {true};
    bool ray_traced_ {true};
    DirtyTracker<GigaVoxel> * tracker_ {};
};

// -----------------------------------------------------------------------------
// GigaVoxelInstance (Scene Object)
// -----------------------------------------------------------------------------
// A GigaVoxel instance in the scene. Occupies a single RenderableIndex slot;
// all internal chunk management is delegated to the GigaVoxel asset. This is
// the ray-tracing / culling entry point for the whole voxel terrain.
class GigaVoxelInstance : public Renderable {
public:
    static TRef<GigaVoxelInstance> Create(Scene * scene, GigaVoxel * giga_voxel, Transform transform = {});

    RenderableHeader GetDeviceRenderableHeader() const override;
    constexpr static RenderableType kRenderableType = RenderableType::kGigaVoxelInstance;

    FORCEINLINE GigaVoxel * GetGigaVoxel() const { return giga_voxel_.Raw(); }

    void Update(RendererView * view, RenderGraphBuilder & builder) override;

    RHIAccelerationStructure * GetBLAS() const override;
    uint32_t GetInstanceCustomIndex() const override;
    uint32_t GetRayTracedClassIndex() const override;
    bool IsEmpty() const override;

    static RayTracedRenderableClassRegistrator<GigaVoxelInstance> kClassRegistrator;

protected:
    GigaVoxelInstance(Scene * scene);
    ~GigaVoxelInstance() override;

    TRef<GigaVoxel> giga_voxel_;
};

MI_NAMESPACE_END

#endif // MI_GIGA_VOXEL_H
