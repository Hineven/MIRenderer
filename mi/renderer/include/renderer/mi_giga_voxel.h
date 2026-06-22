/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_GIGA_VOXEL_H
#define MI_GIGA_VOXEL_H

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <core/base.h>
#include <core/refcounted.h>
#include <renderer/mi_aabb.h>
#include <renderer/mi_dirty_tracker.h>
#include <renderer/mi_giga_voxel_heap.h>
#include <renderer/mi_partition_allocator.h>
#include <renderer/mi_renderable.h>
#include <renderer/mi_renderer_fwd.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_texture.h>
#include <rdg/rdg_ray_tracing_registry.h>

#include "../shaders/shared/SharedGigaVoxel.hlsl"

MI_NAMESPACE_BEGIN

class Scene;

// =============================================================================
// GigaVoxel device-side data layer.
//
// A GigaVoxel asset is the GPU counterpart of a voxel terrain (macromc world).
// It occupies ONE bindless slot (SlotKind::GigaVoxel) and, per the visibility
// buffer design, a single GigaVoxelInstance occupies ONE RenderableIndex slot —
// internal chunk streaming does NOT register thousands of renderables with the
// Scene.
//
// Geometry + per-chunk BLAS ownership:
//   - Vertex/index geometry lives in the GigaVoxelGeometryHeap owned by the
//     GigaVoxel asset (shared across all chunks).
//   - Per-chunk BLASes (one per uploaded chunk) are owned by the GigaVoxel
//     asset, NOT by DeviceGigaVoxel. They are built lazily during the render
//     loop (GigaVoxelInstance::Update) via an RDG pass. TLAS/PTLAS integration
//     lands later; until then GetBLAS() returns null and the per-chunk BLASes
//     are build-verified but not traced against.
//   - A block-texture atlas (4096^2, 256x256 tiles of 16^2) is a GLOBAL config
//     shared by all GigaVoxel assets, referenced by bindless index through
//     GigaVoxelHeader.AtlasBindlessIndex.
// =============================================================================

// -----------------------------------------------------------------------------
// DeviceGigaVoxel
// -----------------------------------------------------------------------------
// Owns the bindless slot for one GigaVoxel asset. Vertex/index geometry lives
// in the GigaVoxelGeometryHeap owned by the GigaVoxel asset; per-chunk BLASes
// also live on the asset. DeviceGigaVoxel is just the bindless slot keeper.
class DeviceGigaVoxel : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    FORCEINLINE bool IsValid() const { return slot_ && slot_->Get() != UINT32_MAX; }
    FORCEINLINE uint32_t GetIndex() const { return slot_ ? slot_->Get() : UINT32_MAX; }

protected:
    DeviceGigaVoxel(DeviceBindlessResourceAllocator * allocator);
    ~DeviceGigaVoxel() override;

    // Bindless slot keeper (delayed free through the allocator).
    TRef<DeviceBindlessResourceAllocator::SlotKeeper> slot_;

    friend class GigaVoxel;
};

// -----------------------------------------------------------------------------
// GigaVoxel (Asset)
// -----------------------------------------------------------------------------
// Host-side voxel terrain asset, parallel to StaticMesh / VolumeGrid. Accepts
// chunk geometry (already converted to the renderer-layer GigaVoxelVertex),
// uploads + builds device resources on demand. Mirrors the StaticMesh pattern:
// the asset knows nothing about its instances; a GigaVoxelInstance (the Scene
// projection) owns a TRef<GigaVoxel> back-pointer. 1:N is supported (each
// instance contributes its own N per-chunk BLAS instances to the TLAS/PTLAS).
//
// Chunk geometry is managed by a GigaVoxelGeometryHeap (size-class allocator):
// each chunk gets a stable {vertex_offset, index_offset} and can be added /
// removed / updated independently in O(1) (plus an incremental GPU upload of
// just that chunk's range). CPU is the source of truth for offsets; the GPU
// buffers mirror them.
//
// Per-chunk BLAS: one BLAS per uploaded chunk, owned here. Indices stay
// chunk-local (0-based); each chunk's BLAS references its own vertex/index span
// (offset+count from the handle). Built lazily in BuildDirtyChunkBLAS_Async,
// driven from GigaVoxelInstance::Update via an RDG pass. TLAS/PTLAS wiring is a
// later phase — until then these BLASes are build-verified but not traced.
//
// The block-texture atlas is a GLOBAL config shared by ALL GigaVoxel assets
// (set once via SetGlobalAtlas at app init).
//
// ChunkId identifies a chunk; the caller picks the id space (e.g. a packed
// chunk coord or a monotonic counter). One id maps to at most one live handle.

// Identifier for a chunk inside a GigaVoxel asset. Opaque 64-bit; the caller
// defines the encoding.
using GigaVoxelChunkId = uint64_t;
static constexpr GigaVoxelChunkId kInvalidGigaVoxelChunkId = UINT64_MAX;

// (Chunk layout convention: size constants, GigaVoxelChunkCoord, and the
//  GigaVoxelChunkWorldOrigin / GigaVoxelChunkLocalAABB helpers are defined in
//  mi_giga_voxel_heap.h, since GigaVoxelChunkHandle embeds GigaVoxelChunkCoord
//  and the heap header is included before this file.)

class GigaVoxel : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    friend class Renderer;
    friend class GigaVoxelInstance;

    static TRef<GigaVoxel> Create();

    // ---- Global atlas (block texture atlas, 4096^2, 256x256 tiles of 16^2) ----
    // Shared by ALL GigaVoxel assets. Call once at app init with a texture that
    // is already uploaded to the device; this converts it to bindless and caches
    // the bindless index. All GigaVoxelHeaders read from this global index.
    // NOTE: ownership lives on DeviceBindlessResourceAllocator (so it dies before
    // the RHI singleton); these statics are thin delegates to it.
    static void SetGlobalAtlas(TRef<Texture> atlas);
    static uint32_t GetGlobalAtlasBindlessIndex();

    // ---- Global geometry heap (shared vertex/index uber buffer) ----
    // All GigaVoxel assets share ONE GigaVoxelGeometryHeap so that a single SRV
    // (GigaVoxelVertexBuffer / GigaVoxelIndexBuffer) covers every asset. This
    // mirrors the StaticMesh pattern (all StaticMeshes share the allocator's
    // global vertex/index uber buffers). Lazily created on first access.
    // NOTE: ownership lives on DeviceBindlessResourceAllocator (so it dies before
    // the RHI singleton); this static is a thin delegate to it.
    static GigaVoxelGeometryHeap * GetGlobalGeometryHeap();
    static GigaVoxelGeometryHeap * GetGeometryHeap() { return GetGlobalGeometryHeap(); }

    // ---- TLAS instance gathering (per-chunk BLAS → N instances) ----
    // shellIndex is the dense index of this asset in its GigaVoxelShellRegistry
    // (0..255). It occupies the low 8 bits of instance customIndex. Set once
    // when the registry registers the asset.
    void SetShellIndex(uint8_t shell_index) { shell_index_ = shell_index; }
    // Monotonic revision bumped on every change that affects the cached
    // per-chunk BLAS instance list (chunk topology add/remove/update/compact, or
    // a BLAS build). GigaVoxelInstance stores the last revision it rebuilt its
    // cached_instances_ from and lazily rebuilds when this differs. Mirrors the
    // global_instances_dirty_ scheme on StaticMeshInstance.
    FORCEINLINE uint64_t GetInstancesRevision() const { return instances_revision_; }

    // ---- Chunk geometry (heap-backed; O(1) alloc/free + incremental upload) ----
    // Add a new chunk. Allocates ranges, uploads to GPU immediately (the queue
    // from UpdateOnDevice is NOT required — a fresh upload uses the global
    // graphics queue), marks the chunk's BLAS dirty. Returns the handle;
    // valid==false on allocation failure (caller may CompactChunks() and retry).
    // If `id` is already live, it is replaced (old range + old BLAS freed first).
    // `coord` is the chunk's 2D grid coordinate; the renderer uses it (plus the
    // compile-time chunk size) to derive the chunk's world placement. Vertices
    // must be CHUNK-LOCAL (origin at the chunk's world corner); indices must be
    // chunk-local (0-based).
    GigaVoxelChunkHandle UploadChunk(GigaVoxelChunkId id,
                                     GigaVoxelChunkCoord coord,
                                     std::vector<GigaVoxelVertex> vertices,
                                     std::vector<uint32_t> indices);
    // Replace an existing chunk's geometry (free old range, allocate new,
    // upload). If `id` is unknown this is equivalent to UploadChunk. `coord`
    // updates the chunk's world placement.
    void UpdateChunk(GigaVoxelChunkId id,
                     GigaVoxelChunkCoord coord,
                     std::vector<GigaVoxelVertex> vertices,
                     std::vector<uint32_t> indices);
    // Remove a chunk and free its ranges + BLAS. No-op if unknown.
    void RemoveChunk(GigaVoxelChunkId id);
    // Remove all chunks.
    void ClearAllChunks();
    // Defragment the geometry heap (low-frequency fallback). Re-uploads moved
    // chunks and invalidates their BLAS (rebuilt on next dirty build). The
    // chunk id->handle map is updated in place.
    void CompactChunks();

    // TODO add batched update commands for UploadChunk & UpdateChunk & RemoveChunk
    // AABB maintenance can be faster.
    // TODO accept BatchedUploadContext as a parameter for these functions
    // batch memory uploads. Making the entire process faster when there're tons of chunks
    // waiting for upload.

    FORCEINLINE DeviceGigaVoxel * GetDeviceGigaVoxel() const { return device_giga_voxel_.Raw(); }
    // GetGeometryHeap() now returns the global heap (see static accessor above).
    FORCEINLINE bool IsEmpty() const { return chunk_handles_.empty(); }
    FORCEINLINE AABB GetAABB() const { return aabb_; }

    // Read-only access to the chunk id->handle map. Used by the renderer to
    // build per-chunk raster draw commands (each handle carries the chunk's
    // vertex/index span + global chunk-header index in the global heap).
    const std::unordered_map<GigaVoxelChunkId, GigaVoxelChunkHandle> & GetChunkHandles() const {
        return chunk_handles_;
    }

    // True if any chunk's BLAS needs (re)building. Polled by
    // GigaVoxelInstance::Update to decide whether to emit the BLAS-build pass.
    FORCEINLINE bool HasDirtyBLAS() const { return !blas_dirty_.empty(); }

    // Build the per-chunk BLAS for every dirty chunk. Recording commands go to
    // the given graphics queue; caller is responsible for submission +
    // synchronization. The BLAS RHI objects are created lazily here (CPU side,
    // so GetBLAS-style handles are valid immediately after) and the build
    // commands are recorded for GPU execution. Also refreshes the
    // GigaVoxelHeader. No-op if nothing is dirty.
    void BuildDirtyChunkBLAS_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue);
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

    // Bump the instances revision. Called by every chunk topology change
    // (add/remove/update/compact) and after a BLAS build so attached
    // GigaVoxelInstances know to lazily rebuild their cached_instances_.
    void BumpInstancesRevision();

    // Per-asset slot (customIndex high 16 bits) + global partition id lifecycle.
    // Acquire: assign a fresh/recycled slot + allocate a partition (if allocator
    // set). Release: recycle the slot + free the partition.
    void AcquireChunkSlotAndPartition(GigaVoxelChunkId id);
    void ReleaseChunkSlotAndPartition(GigaVoxelChunkId id);

    TRef<DeviceGigaVoxel> device_giga_voxel_;
    // NOTE: geometry heap is now global (GetGlobalGeometryHeap). Kept here only
    // as the static storage owner; do not add a per-asset member.

    // chunk id -> live handle. Drives the heap and the per-chunk BLAS span.
    std::unordered_map<GigaVoxelChunkId, GigaVoxelChunkHandle> chunk_handles_;
    // chunk id -> per-chunk BLAS. Built lazily in BuildDirtyChunkBLAS_Async.
    std::unordered_map<GigaVoxelChunkId, TRef<RHIAccelerationStructure>> chunk_BLAS_;
    // chunk ids whose BLAS needs (re)building this frame.
    std::unordered_set<GigaVoxelChunkId> blas_dirty_;
    // chunk id -> partition id (one partition per chunk). Allocated from the
    // renderer's PartitionAllocator; freed on chunk removal.
    std::unordered_map<GigaVoxelChunkId, uint32_t> chunk_partitions_;
    // chunk id -> chunk-local AABB (the chunk's geometry bounds in chunk-local
    // space, i.e. local origin = the chunk's world corner). Maintained for
    // incremental aabb updates and per-chunk world AABB derivation (local AABB
    // translated by GigaVoxelChunkWorldOrigin).
    std::unordered_map<GigaVoxelChunkId, AABB> chunk_aabbs_;
    // chunk id -> 2D grid coordinate. Drives the per-chunk world placement
    // (chunk-local vertices -> world via GigaVoxelChunkWorldOrigin(coord)).
    std::unordered_map<GigaVoxelChunkId, GigaVoxelChunkCoord> chunk_coords_;
    // chunk id -> per-asset dense slot (0..65535, used in customIndex high bits).
    // A monotonic counter + free list keeps slots dense and reusable.
    std::unordered_map<GigaVoxelChunkId, uint16_t> chunk_slots_;
    SlotAllocator chunk_slots_allocator_;
    // Monotonic revision. Bumped whenever the cached per-chunk BLAS instance
    // list would change. Attached GigaVoxelInstances compare against their last
    // seen revision and lazily rebuild.
    uint64_t instances_revision_ {0};
    // Dense index of this asset in its GigaVoxelShellRegistry (low 8 bits of
    // instance customIndex). Set by the registry.
    uint8_t shell_index_ {0};

    // NOTE: the global atlas + geometry heap previously lived here as class-statics.
    // They moved to DeviceBindlessResourceAllocator (owned there so they die before
    // the RHI singleton). Access via GetGlobalGeometryHeap()/GetGlobalAtlasBindlessIndex(),
    // which delegate to Renderer::Get().GetDeviceAllocator().

    AABB aabb_ {}; // World-space AABB (union of per-chunk world AABBs)
    bool dirty_ {true};
    bool ray_traced_ {true};
    DirtyTracker<GigaVoxel> * tracker_ {};

    constexpr static auto kMaxNumChunkSlots = 65536;
};

// -----------------------------------------------------------------------------
// GigaVoxelInstance (Scene Object)
// -----------------------------------------------------------------------------
// A GigaVoxel instance in the scene. Occupies a single RenderableIndex slot;
// all internal chunk management is delegated to the GigaVoxel asset. This is
// the ray-tracing / culling entry point for the whole voxel terrain.
//
// Ownership (mirrors StaticMesh / StaticMeshInstance): the instance OWNS a
// TRef<GigaVoxel> to its asset. The asset knows nothing about its instance(s);
// 1:N is supported. When the instance is released (renderable delayed-free),
// its TRef drops the asset, which in turn frees its device resources.
//
// The per-chunk BLAS instance list (one RenderableBLASInstance per uploaded
// chunk, with this instance's RenderableIndex baked into customIndex) is cached
// here and rebuilt lazily when the asset's instances_revision changes.
class GigaVoxelInstance : public Renderable {
public:
    static TRef<GigaVoxelInstance> Create(Scene * scene, TRef<GigaVoxel> giga_voxel, Transform transform = {});

    RenderableHeader GetDeviceRenderableHeader() const override;
    constexpr static RenderableType kRenderableType = RenderableType::kGigaVoxelInstance;

    FORCEINLINE GigaVoxel * GetGigaVoxel() const { return giga_voxel_.Raw(); }

    void Update(RendererView * view, RenderGraphBuilder & builder) override;

    // Per-chunk BLAS instances. The instance itself is a single RenderableIndex
    // slot but contributes N BLAS instances to the TLAS/PTLAS (one per chunk).
    // Lazily rebuilt when the asset's instances_revision advances.
    std::span<const RenderableBLASInstance> GetPartitionedBLASInstances () const override;

    RHIAccelerationStructure * GetBLAS() const override;
    uint32_t GetInstanceCustomIndex() const override;
    uint32_t GetRayTracedClassIndex() const override;
    bool IsEmpty() const override;

    static RayTracedRenderableClassRegistrator<GigaVoxelInstance> kClassRegistrator;

protected:
    GigaVoxelInstance(Scene * scene);
    ~GigaVoxelInstance() override;

    // Rebuild cached_instances_ from the asset's chunk_handles_ + chunk_BLAS_ +
    // chunk_partitions_ + chunk_aabbs_ + chunk_coords_. Each chunk with a built
    // BLAS becomes one instance (customIndex encodes this instance's
    // RenderableIndex + the chunk's global header index; transform = the chunk's
    // world placement from its coord, since vertices are chunk-local; partition
    // from the allocator). Chunks without a built BLAS yet are skipped. Also
    // recomputes this instance's world AABB. Accesses asset privates via friend.
    void RebuildCachedInstances();

    // Owning. Keeps the asset alive for the instance's lifetime.
    TRef<GigaVoxel> giga_voxel_;

    // Slot in the global GigaVoxelInstanceRTHeaderBuffer side table. Acquired at
    // construction, released (delayed via the renderable free path) at destruction.
    // Encoded into the high 8 bits of every per-chunk RT InstanceCustomIndex so RT
    // hit shaders can reach this instance's RenderableIndex + GigaVoxelIndex.
    uint32_t rt_header_slot_ {0xFFFFFFFFu};

    // Cached per-chunk BLAS instances for TLAS gathering. Lazily rebuilt in
    // GetPartitionedBLASInstances when the asset's revision advances.
    mutable std::vector<RenderableBLASInstance> cached_instances_;
    mutable uint64_t instances_revision_seen_ {static_cast<uint64_t>(-1)};
};

MI_NAMESPACE_END

#endif // MI_GIGA_VOXEL_H
