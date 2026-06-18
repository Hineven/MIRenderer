/*
 * Created: 2026/06/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <renderer/mi_giga_voxel.h>

#include <ranges>

#include <rdg/rdg_builder.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_buffer_heap.h>
#include <renderer/mi_renderer.h>
#include <renderer/mi_scene.h>
#include <rhi/rhi.h>
#include <rhi/rhi_as.h>

#include "shaders/shared/SharedRenderable.hlsl"
#include "rdg/rdg_ray_tracing_registry.h"

MI_NAMESPACE_BEGIN

// ============================= DeviceGigaVoxel =============================
DeviceGigaVoxel::DeviceGigaVoxel(DeviceBindlessResourceAllocator * allocator) {
    slot_ = allocator->AllocateGigaVoxelSlotKeeper();
}

DeviceGigaVoxel::~DeviceGigaVoxel() {
    // Slot is freed (delayed) by the SlotKeeper. Geometry lives in the
    // GigaVoxelGeometryHeap (owned by GigaVoxel); per-chunk BLASes are also on
    // the GigaVoxel asset — neither lives here.
}

// ================================ GigaVoxel =================================
// Global atlas state (process-wide; shared by all GigaVoxel assets).
TRef<Texture> GigaVoxel::global_atlas_;
uint32_t GigaVoxel::global_atlas_bindless_index_ = 0xFFFFFFFFu;

GigaVoxel::GigaVoxel() {
    // GPU usage flags for the geometry heap: vertex/index + storage + AS build
    // input + shader device address (matches the old dedicated uber buffers).
    auto usage = RHIBufferUsageFlagBits::kVertex | RHIBufferUsageFlagBits::kIndex
               | RHIBufferUsageFlagBits::kStorage
               | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput
               | RHIBufferUsageFlagBits::kShaderDeviceAddress;
    geometry_heap_ = new GigaVoxelGeometryHeap(usage, usage);
}

GigaVoxel::~GigaVoxel() {
    // Release the instance first so it unregisters from the scene before the
    // asset (its back-pointer target) goes away.
    DetachFromScene();
}

TRef<GigaVoxel> GigaVoxel::Create() {
    return TRef(new GigaVoxel());
}

void GigaVoxel::SetGlobalAtlas(TRef<Texture> atlas) {
    global_atlas_ = atlas;
    global_atlas_bindless_index_ = global_atlas_ ? global_atlas_->GetBindlessIndex() : 0xFFFFFFFFu;
}

TRef<Texture> GigaVoxel::GetGlobalAtlas() {
    return global_atlas_;
}

uint32_t GigaVoxel::GetGlobalAtlasBindlessIndex() {
    return global_atlas_bindless_index_;
}

void GigaVoxel::SetDirty(bool dirty) {
    dirty_ = dirty;
    if (tracker_ && dirty_) tracker_->OnObjectTurnedDirty(this);
}

void GigaVoxel::AttachToScene(Scene * scene, Transform transform) {
    if (instance_) return;  // already attached
    if (!scene) return;
    instance_ = GigaVoxelInstance::Create(scene, this, transform);
}

void GigaVoxel::DetachFromScene() {
    instance_ = {};  // releases the TRef -> renderable delayed-free path
}

void GigaVoxel::AcquireChunkSlotAndPartition(GigaVoxelChunkId id) {
    // Per-asset dense slot (for customIndex high 16 bits).
    uint16_t slot;
    if (!chunk_slots_free_.empty()) {
        slot = chunk_slots_free_.back();
        chunk_slots_free_.pop_back();
    } else {
        mi_assert(chunk_slots_.size() < 65536, "GigaVoxel chunk slot overflow (>65535).");
        slot = static_cast<uint16_t>(chunk_slots_.size());
    }
    chunk_slots_[id] = slot;
    // Global partition id (for PTLAS). Bookkeeping-only until PTLAS RHI lands.
    if (partition_allocator_) {
        chunk_partitions_[id] = partition_allocator_->AllocatePartition();
    }
}

void GigaVoxel::ReleaseChunkSlotAndPartition(GigaVoxelChunkId id) {
    auto sit = chunk_slots_.find(id);
    if (sit != chunk_slots_.end()) {
        chunk_slots_free_.push_back(sit->second);
        chunk_slots_.erase(sit);
    }
    auto pit = chunk_partitions_.find(id);
    if (pit != chunk_partitions_.end()) {
        if (partition_allocator_) partition_allocator_->FreePartition(pit->second);
        chunk_partitions_.erase(pit);
    }
}

void GigaVoxel::RebuildCachedInstances() {
    cached_instances_.clear();
    cached_instances_.reserve(chunk_handles_.size());
    for (const auto & [id, h] : chunk_handles_) {
        auto bit = chunk_BLAS_.find(id);
        if (bit == chunk_BLAS_.end() || !bit->second) continue;  // BLAS not built yet
        auto sit = chunk_slots_.find(id);
        if (sit == chunk_slots_.end()) continue;
        auto pit = chunk_partitions_.find(id);
        RenderableBLASInstance inst {};
        inst.blas = bit->second.Raw();
        inst.transform = {};  // identity (vertices are world-space)
        // customIndex = shellIndex(8) | chunkSlot(16) << 8
        inst.instance_custom_index = static_cast<uint32_t>(shell_index_)
                                   | (static_cast<uint32_t>(sit->second) << 8);
        inst.instance_mask = 0xFF;
        inst.instance_contribution_to_hit_group_index =
            GigaVoxelInstance::kClassRegistrator.GetClassIndex();
        inst.partition_index = (pit != chunk_partitions_.end()) ? pit->second : 0;
        inst.explicit_aabb = {};  // TODO: per-chunk AABB (cheap to add later)
        cached_instances_.push_back(inst);
    }
}

std::span<const RenderableBLASInstance> GigaVoxel::GetPartitionedBLASInstances() const {
    return std::span<const RenderableBLASInstance>(cached_instances_);
}

// Recompute the asset AABB from all live chunk vertex data. Called after any
// chunk topology change (add/remove/clear/compact).
static void RecomputeAABB(const std::unordered_map<GigaVoxelChunkId, GigaVoxelChunkHandle> & handles,
                          const GigaVoxelGeometryHeap * heap, AABB & out) {
    out = AABB::Empty();
    if (!heap) return;
    auto verts = heap->GetCPUVertices();
    for (const auto & [id, h] : handles) {
        if (!h.valid || h.IsEmpty()) continue;
        for (uint32_t i = 0; i < h.vertex_count; ++i) {
            out.Encapsulate(verts[h.vertex_offset + i].position);
        }
    }
}

GigaVoxelChunkHandle GigaVoxel::UploadChunk(GigaVoxelChunkId id,
                                             std::vector<GigaVoxelVertex> vertices,
                                             std::vector<uint32_t> indices) {
    // Replace existing entry first (free old range + drop its BLAS + slot + partition).
    if (auto it = chunk_handles_.find(id); it != chunk_handles_.end()) {
        geometry_heap_->FreeChunk(it->second);
        chunk_handles_.erase(it);
        chunk_BLAS_.erase(id);
        ReleaseChunkSlotAndPartition(id);
    }

    GigaVoxelChunkHandle h = geometry_heap_->AllocateChunk(
        static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(indices.size()));
    if (!h.valid) {
        // Fall back: compact and retry once.
        geometry_heap_->Compact();
        h = geometry_heap_->AllocateChunk(
            static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(indices.size()));
    }
    if (!h.valid) {
        SetDirty();
        return h;
    }
    // Indices are chunk-local (0-based). Each chunk's BLAS references its own
    // vertex/index span (offset+count from the handle), so NO heap-global
    // rebias is applied (unlike the old single-merged-BLAS arrangement).
    geometry_heap_->UpdateChunk(h, vertices, indices);
    chunk_handles_[id] = h;
    // Assign a per-asset slot (for customIndex) + a global partition id (for
    // PTLAS). Both are stable until this chunk is removed.
    AcquireChunkSlotAndPartition(id);
    // This chunk's geometry changed -> its BLAS must be (re)built.
    blas_dirty_.insert(id);
    RecomputeAABB(chunk_handles_, geometry_heap_.Raw(), aabb_);
    RebuildCachedInstances();
    SetDirty();
    return h;
}

void GigaVoxel::UpdateChunk(GigaVoxelChunkId id,
                            std::vector<GigaVoxelVertex> vertices,
                            std::vector<uint32_t> indices) {
    // UploadChunk already replaces an existing id, so delegate.
    UploadChunk(id, std::move(vertices), std::move(indices));
}

void GigaVoxel::RemoveChunk(GigaVoxelChunkId id) {
    auto it = chunk_handles_.find(id);
    if (it == chunk_handles_.end()) return;
    geometry_heap_->FreeChunk(it->second);
    chunk_handles_.erase(it);
    chunk_BLAS_.erase(id);
    blas_dirty_.erase(id);
    ReleaseChunkSlotAndPartition(id);
    RecomputeAABB(chunk_handles_, geometry_heap_.Raw(), aabb_);
    RebuildCachedInstances();
    SetDirty();
}

void GigaVoxel::ClearAllChunks() {
    for (auto & [id, h] : chunk_handles_) geometry_heap_->FreeChunk(h);
    chunk_handles_.clear();
    chunk_BLAS_.clear();
    blas_dirty_.clear();
    for (auto & [id, slot] : chunk_slots_) {
        (void)slot;
        if (partition_allocator_) {
            auto pit = chunk_partitions_.find(id);
            if (pit != chunk_partitions_.end()) partition_allocator_->FreePartition(pit->second);
        }
    }
    chunk_slots_.clear();
    chunk_slots_free_.clear();
    chunk_partitions_.clear();
    cached_instances_.clear();
    aabb_ = AABB::Empty();
    SetDirty();
}

void GigaVoxel::CompactChunks() {
    // Chunk-granular compaction: snapshot every live chunk's geometry, clear
    // the heaps, then re-allocate contiguously in a stable order. Indices stay
    // chunk-local (no rebias), so this is simpler than the old arrangement: we
    // just copy vertices + indices verbatim per chunk. O(chunks) — low-
    // frequency fallback. Every chunk's BLAS is invalidated (vertex_offset
    // changed) and rebuilt on the next dirty build.
    if (chunk_handles_.empty()) return;

    // Snapshot {id, vertices, indices (chunk-local)} in chunk-handle order.
    struct Snap { GigaVoxelChunkId id; std::vector<GigaVoxelVertex> v; std::vector<uint32_t> i; };
    std::vector<Snap> snaps;
    snaps.reserve(chunk_handles_.size());
    auto cpu_v = geometry_heap_->GetCPUVertices();
    auto cpu_i = geometry_heap_->GetCPUIndices();
    for (const auto & [id, h] : chunk_handles_) {
        if (!h.valid || h.IsEmpty()) continue;
        Snap s;
        s.id = id;
        s.v.assign(cpu_v.begin() + h.vertex_offset, cpu_v.begin() + h.vertex_offset + h.vertex_count);
        s.i.assign(cpu_i.begin() + h.index_offset, cpu_i.begin() + h.index_offset + h.index_count);
        snaps.push_back(std::move(s));
    }

    // Wipe both heaps, the handle map, and all per-chunk BLASes (offsets moved).
    for (auto & [id, h] : chunk_handles_) geometry_heap_->FreeChunk(h);
    chunk_handles_.clear();
    chunk_BLAS_.clear();
    blas_dirty_.clear();
    // Slots stay (chunk identity unchanged); partitions stay (still live).
    // Only the geometry moved, so just mark every re-allocated chunk BLAS dirty.

    // Re-allocate contiguously (no fragmentation). No queue here: GPU upload is
    // reconciled on the next BuildDirtyChunkBLAS_Async. Pass null queue to
    // UpdateChunk for CPU-only writes.
    for (auto & s : snaps) {
        GigaVoxelChunkHandle h = geometry_heap_->AllocateChunk(
            static_cast<uint32_t>(s.v.size()), static_cast<uint32_t>(s.i.size()));
        mi_assert(h.valid, "CompactChunks re-alloc failed (should always fit).");
        geometry_heap_->UpdateChunk(h, s.v, s.i, /*queue*/ nullptr);
        chunk_handles_[s.id] = h;
        blas_dirty_.insert(s.id);  // new offset -> BLAS needs rebuild
    }
    RecomputeAABB(chunk_handles_, geometry_heap_.Raw(), aabb_);
    RebuildCachedInstances();
    SetDirty();
}

void GigaVoxel::BuildDirtyChunkBLAS_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    // Nothing to do if no chunk changed AND the device header is already up to
    // date. The header write below is gated on `dirty_` (set by any topology
    // change); the per-chunk BLAS builds are gated on blas_dirty_.
    if (!dirty_ && blas_dirty_.empty()) return;

    // Lazy device-object creation (bindless slot).
    if (!device_giga_voxel_) {
        device_giga_voxel_ = new DeviceGigaVoxel(alloc);
        mi_check(device_giga_voxel_->IsValid(), "Failed to allocate GigaVoxel slot.");
    }

    // ---- Refresh the GigaVoxelHeader (covers the whole live heap range) ----
    // VertexOffset/IndexOffset are 0 + the heap watermark; per-chunk addressing
    // is handled at the visibility-buffer / TLAS level in later phases. The
    // shader still reads a single merged range for now.
    if (dirty_) {
        size_t v_count = geometry_heap_->GetVertexHighWatermark();
        size_t i_count = geometry_heap_->GetIndexHighWatermark();
        GigaVoxelHeader header {};
        header.VertexOffset = 0;
        header.IndexOffset  = 0;
        header.VertexCount  = static_cast<uint32_t>(v_count);
        header.IndexCount   = static_cast<uint32_t>(i_count);
        header.AtlasBindlessIndex = global_atlas_bindless_index_;
        Helpers::Upload_Async(queue, alloc->GetGigaVoxelHeaderBuffer(),
                              sizeof(GigaVoxelHeader) * device_giga_voxel_->GetIndex(), header);
    }

    // ---- Build per-chunk BLAS for every dirty chunk ----
    // Each chunk's BLAS references its OWN vertex/index span (offset+count from
    // the handle). Indices are chunk-local (0-based). The BLAS RHI object is
    // created lazily (CPU side, so the handle is valid immediately); build
    // commands are recorded for GPU execution.
    if (ray_traced_) {
        auto gpu_vbuf = geometry_heap_->GetGPUVertexBuffer();
        auto gpu_ibuf = geometry_heap_->GetGPUIndexBuffer();
        for (GigaVoxelChunkId id : blas_dirty_) {
            auto it = chunk_handles_.find(id);
            if (it == chunk_handles_.end()) continue;          // was removed
            const GigaVoxelChunkHandle & h = it->second;
            if (!h.valid || h.IsEmpty()) {
                chunk_BLAS_.erase(id);
                continue;
            }
            // Lazy-create the per-chunk BLAS RHI object.
            TRef<RHIAccelerationStructure> & blas_slot = chunk_BLAS_[id];
            if (!blas_slot) {
                blas_slot = RHI::Get().CreateAccelerationStructure(RHIAccelerationStructureType::kBottomLevel);
            }
            auto as_geom = queue.Allocate<RHIASGeometry>();
            *as_geom = RHIASGeometry{
                RHIASGeometryType::kTriangles,
                RHIASGeometryFlagBits::kOpaque, // VC geometry is opaque (no transparent pass yet)
                {
                    RHIBufferSpan{gpu_vbuf, h.vertex_offset * sizeof(GigaVoxelVertex),
                                  h.vertex_count * sizeof(GigaVoxelVertex)},
                    sizeof(GigaVoxelVertex),
                    h.vertex_count,
                    RHIVertexAttributeFormatType::k3xFp32,
                    RHIBufferSpan{gpu_ibuf, h.index_offset * sizeof(uint32_t),
                                  h.index_count * sizeof(uint32_t)},
                    h.index_count,
                    RHIIndexType::kUint32
                }
            };
            auto build_info = RHIAccelerationStructureBuildGeometryInfo{
                RHIAccelerationStructureType::kBottomLevel,
                RHIAccelerationStructureBuildFlagBits::kPreferFastTrace,
                RHIAccelerationStructureBuildMode::kBuild,
                {},
                blas_slot.Raw(),
                {as_geom, 1}, {}, {}
            };
            auto sizes = blas_slot->GetBuildSizes(build_info);
            blas_slot->Create(sizes.acceleration_structure_size);
            auto scratch = RHI::Get().CreateBuffer(sizes.build_scratch_size,
                                                   RHIBufferUsageFlagBits::kAccelerationStructureScratch);
            queue.BuildAccelerationStructure(build_info, scratch->GetSpan());
            queue.AccelerationStructureBarrier(
                blas_slot.Raw(),
                RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIPipelineStageFlagBits::kRayTracing | RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIGPUAccessFlagBits::kAccelerationStructureWrite,
                RHIGPUAccessFlagBits::kAccelerationStructureRW);
        }
    }
    blas_dirty_.clear();
    // Chunks may have gained/lost their BLAS this build -> refresh the cached
    // instance list so TLAS gathering sees the up-to-date set.
    RebuildCachedInstances();
    SetDirty(false);
}

void GigaVoxel::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    BuildDirtyChunkBLAS_Async(alloc, queue);
    queue.WaitForIdle("GigaVoxel::UpdateOnDevice");
}

// ============================ GigaVoxelInstance =============================
GigaVoxelInstance::GigaVoxelInstance(Scene * scene)
    : Renderable(RenderableType::kGigaVoxelInstance, scene) {}

GigaVoxelInstance::~GigaVoxelInstance() = default;

TRef<GigaVoxelInstance> GigaVoxelInstance::Create(Scene * scene, GigaVoxel * giga_voxel, Transform transform) {
    auto inst = TRef(new GigaVoxelInstance(scene));
    if (inst->IsValid()) {
        inst->SetTransform(transform);
        inst->scene_ = scene;
        inst->giga_voxel_ = giga_voxel;   // non-owning back-pointer; asset owns us
        return std::move(inst);
    }
    return {};
}

void GigaVoxelInstance::Update(RendererView * view, RenderGraphBuilder & builder) {
    aabb_ = giga_voxel_ ? giga_voxel_->GetAABB() : AABB::Empty();

    // Build per-chunk BLAS for any dirty chunks. renderer touches RHI directly
    // (the queue buffers commands in submission order from the render thread),
    // so BLAS build commands land before the TLAS gather later this frame.
    if (giga_voxel_ && giga_voxel_->HasDirtyBLAS()) {
        auto * alloc = Renderer::Get().GetDeviceAllocator();
        auto & queue = RHI::Get().GetGraphicsCommandQueue();
        giga_voxel_->BuildDirtyChunkBLAS_Async(alloc, queue);
        SetBLASUpdated(true);
    }
    SetDirty(false);
}

std::span<const RenderableBLASInstance> GigaVoxelInstance::GetPartitionedBLASInstances() const {
    return giga_voxel_ ? giga_voxel_->GetPartitionedBLASInstances() : std::span<const RenderableBLASInstance>{};
}

RenderableHeader GigaVoxelInstance::GetDeviceRenderableHeader() const {
    return std::bit_cast<RenderableHeader>(GigaVoxelInstanceHeader{
        giga_voxel_ && giga_voxel_->GetDeviceGigaVoxel() ? giga_voxel_->GetDeviceGigaVoxel()->GetIndex() : 0xFFFFFFFFu,
        0, 0, GetRenderableFlags()
    });
}

RHIAccelerationStructure * GigaVoxelInstance::GetBLAS() const {
    // Per-chunk BLASes are owned by the GigaVoxel asset, not exposed here. A
    // TLAS/PTLAS over those chunk BLASes lands in a later phase; until then the
    // instance contributes no single BLAS to the scene TLAS.
    return nullptr;
}

RayTracedRenderableClassRegistrator<GigaVoxelInstance> GigaVoxelInstance::kClassRegistrator("GigaVoxel", "GigaVoxel");

uint32_t GigaVoxelInstance::GetRayTracedClassIndex() const {
    return kClassRegistrator.GetClassIndex();
}

uint32_t GigaVoxelInstance::GetInstanceCustomIndex() const {
    return GetIndex() | (GetRayTracedClassIndex() << Renderable::kRenderableIndexNumBits);
}

bool GigaVoxelInstance::IsEmpty() const {
    return !giga_voxel_ || giga_voxel_->IsEmpty();
}

MI_NAMESPACE_END
