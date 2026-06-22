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
// The global atlas + geometry heap now live on DeviceBindlessResourceAllocator
// (owned there so they are destroyed before the RHI singleton, fixing the exit
// crash from static-destruction order). These statics are thin delegates.

GigaVoxelGeometryHeap * GigaVoxel::GetGlobalGeometryHeap() {
    return Renderer::Get().GetDeviceAllocator()->GetGigaVoxelGeometryHeap();
}

GigaVoxel::GigaVoxel(): chunk_slots_allocator_(kMaxNumChunkSlots) {
    // Ensure the global geometry heap exists (lazily created on first access).
    (void) GetGlobalGeometryHeap();
}

TRef<GigaVoxel> GigaVoxel::Create() {
    return TRef(new GigaVoxel());
}

void GigaVoxel::SetGlobalAtlas(TRef<Texture> atlas) {
    Renderer::Get().GetDeviceAllocator()->SetGigaVoxelAtlas(std::move(atlas));
}

uint32_t GigaVoxel::GetGlobalAtlasBindlessIndex() {
    return Renderer::Get().GetDeviceAllocator()->GetGigaVoxelAtlasBindlessIndex();
}

void GigaVoxel::SetDirty(bool dirty) {
    if (tracker_ && !dirty_ && dirty) {
        tracker_->OnObjectTurnedDirty(this);
    }
    dirty_ = dirty;
}

void GigaVoxel::BumpInstancesRevision() { ++instances_revision_; }

void GigaVoxel::AcquireChunkSlotAndPartition(GigaVoxelChunkId id) {
    // Per-asset dense slot (for customIndex high 16 bits).
    auto slot = chunk_slots_allocator_.AllocateSlot();
    mi_assert(slot != UINT32_MAX, "GigaVoxel chunk slot pool exhausted");
    chunk_slots_[id] = slot;
    // For VC chunks, each chunk is a partition.
    // TODO more types of chunks have different behavior on partitioning
    if (true) {
        chunk_partitions_[id] = Renderer::Get().GetDeviceAllocator()->GetPartitionAllocator()->AllocatePartition();
    }
}

void GigaVoxel::ReleaseChunkSlotAndPartition(GigaVoxelChunkId id) {
    chunk_slots_allocator_.FreeSlot(chunk_slots_[id]);
    chunk_slots_.erase(id);
    // For VC chunks, each chunk is a partition.
    if (true) {
        auto pit = chunk_partitions_.find(id);
        // TODO more types of chunks have different behavior on partitioning.
        if (pit != chunk_partitions_.end()) {
            Renderer::Get().GetDeviceAllocator()->GetPartitionAllocator()->FreePartition(pit->second);
            chunk_partitions_.erase(pit);
        }
    }
}

// Epsilon for geometry validity guards. Degenerate triangles (area below this)
// must NOT reach the renderer layer — the producer (greedy mesher) is contracted
// to skip zero-area quads. A BLAS built over sliver/coincident triangles can
// yield a malformed BVH that hard-hangs the driver during the partitioned-TLAS
// build or traversal. This check is DEBUG-ONLY: in release the contract is
// trusted (filtering in the renderer would be wasted perf on every upload).
constexpr float kGigaVoxelGeometryEps = 1e-6f;

// Debug-only contract check: no degenerate (zero/near-zero-area) triangles and
// no out-of-range indices in the chunk geometry handed to the renderer. The
// producer must guarantee this; this just catches violations early in debug.
static void DebugAssertChunkGeometryValid(const std::vector<GigaVoxelVertex> & vertices,
                                          const std::vector<uint32_t> & indices) {
#ifndef NDEBUG
    if (indices.empty() || vertices.empty()) return;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t ia = indices[i], ib = indices[i + 1], ic = indices[i + 2];
        mi_assert(ia < vertices.size() && ib < vertices.size() && ic < vertices.size(),
                  "GigaVoxel chunk index out of range (producer bug).");
        glm::vec3 e1 = vertices[ib].position - vertices[ia].position;
        glm::vec3 e2 = vertices[ic].position - vertices[ia].position;
        float area_sq = glm::dot(glm::cross(e1, e2), glm::cross(e1, e2));
        mi_assert(area_sq >= kGigaVoxelGeometryEps * kGigaVoxelGeometryEps,
                  "GigaVoxel chunk contains a degenerate triangle (producer must skip zero-area quads).");
    }
#endif
}

// Recompute the full asset world AABB from all live chunks' local AABBs (each
// translated to world by its chunk origin). This is the last-resort fallback
// for incremental maintenance; prefer the incremental path in UploadChunk /
// RemoveChunk. Operates purely on the per-chunk local AABBs + coords (no vertex
// scan), so it does NOT depend on the geometry heap.
static void RecomputeAABB(const std::unordered_map<GigaVoxelChunkId, AABB> & local_aabbs,
                          const std::unordered_map<GigaVoxelChunkId, GigaVoxelChunkCoord> & coords,
                          AABB & out) {
    out = AABB::Empty();
    for (const auto & [id, local] : local_aabbs) {
        auto cit = coords.find(id);
        if (cit == coords.end()) continue;
        glm::vec3 origin = GigaVoxelChunkWorldOrigin(cit->second);
        out.Encapsulate(AABB(local.min + origin, local.max + origin));
    }
}

GigaVoxelChunkHandle GigaVoxel::UploadChunk(GigaVoxelChunkId id,
                                             GigaVoxelChunkCoord coord,
                                             std::vector<GigaVoxelVertex> vertices,
                                             std::vector<uint32_t> indices) {
    // Debug-only contract check: the producer (greedy mesher) must not hand the
    // renderer degenerate triangles — a BLAS over them can hard-hang the driver.
    // Release builds trust the contract (no per-upload filtering cost).
    DebugAssertChunkGeometryValid(vertices, indices);

    // Replace existing entry first (free old range + drop its BLAS + slot + partition).
    if (auto it = chunk_handles_.find(id); it != chunk_handles_.end()) {
        GetGlobalGeometryHeap()->FreeChunk(it->second);
        chunk_handles_.erase(it);
        chunk_BLAS_.erase(id);
        ReleaseChunkSlotAndPartition(id);
    }

    GigaVoxelChunkHandle h = GetGlobalGeometryHeap()->AllocateChunk(
        static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(indices.size()));
    if (!h.valid) {
        // Fall back: compact and retry once.
        GetGlobalGeometryHeap()->Compact();
        h = GetGlobalGeometryHeap()->AllocateChunk(
            static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(indices.size()));
    }
    if (!h.valid) {
        SetDirty();
        return h;
    }
    // Stamp the chunk's grid coord onto the handle (the heap writes the derived
    // world origin into the per-chunk header row from it).
    h.coord = coord;
    // Indices are chunk-local (0-based). Each chunk's BLAS references its own
    // vertex/index span (offset+count from the handle), so NO heap-global
    // rebias is applied (unlike the old single-merged-BLAS arrangement).
    // Upload to GPU immediately (geometry + per-chunk header row) so the data
    // is valid before any BLAS build / raster / RT access this frame.
    auto & upload_queue = RHI::Get().GetGraphicsCommandQueue();
    GetGlobalGeometryHeap()->UpdateChunk(h, vertices, indices, &upload_queue);
    GetGlobalGeometryHeap()->UpdateChunkHeader(h, &upload_queue);
    chunk_handles_[id] = h;
    chunk_coords_[id] = coord;
    // Assign a per-asset slot (for customIndex) + a global partition id (for
    // PTLAS). Both are stable until this chunk is removed.
    AcquireChunkSlotAndPartition(id);
    // Per-chunk LOCAL AABB from uploaded vertices (vertices are chunk-local, so
    // this is the bounds in the chunk's local space). The world AABB for PTLAS
    // explicit_aabb is this translated by the chunk's world origin.
    AABB chunk_local_aabb {}, old_chunk_world_aabb = AABB::Empty();
    bool had_old = chunk_aabbs_.count(id) && chunk_coords_.count(id);
    if (had_old) {
        glm::vec3 old_origin = GigaVoxelChunkWorldOrigin(chunk_coords_[id]);
        AABB old_local = chunk_aabbs_[id];
        old_chunk_world_aabb = AABB(old_local.min + old_origin, old_local.max + old_origin);
    }
    if (!vertices.empty()) {
        chunk_local_aabb.min = vertices[0].position;
        chunk_local_aabb.max = vertices[0].position;
        for (const auto & v : vertices) {
            chunk_local_aabb.min = glm::min(chunk_local_aabb.min, v.position);
            chunk_local_aabb.max = glm::max(chunk_local_aabb.max, v.position);
        }
    }
    chunk_aabbs_[id] = chunk_local_aabb;
    glm::vec3 origin = GigaVoxelChunkWorldOrigin(coord);
    AABB chunk_world_aabb(chunk_local_aabb.min + origin, chunk_local_aabb.max + origin);
    // This chunk's geometry changed -> its BLAS must be (re)built.
    blas_dirty_.insert(id);
    auto min_aabb_border = glm::equal(old_chunk_world_aabb.min, aabb_.min);
    auto max_aabb_border = glm::equal(old_chunk_world_aabb.max, aabb_.max);
    if (had_old && (glm::any(min_aabb_border) || glm::any(max_aabb_border))) {
        auto greater_min = glm::lessThan(old_chunk_world_aabb.min, chunk_world_aabb.min);
        auto smaller_max = glm::lessThan(chunk_world_aabb.max, old_chunk_world_aabb.max);
        if (glm::any(min_aabb_border & greater_min) || glm::any(max_aabb_border & smaller_max)) {
            // The chunk shrank but the old aabb border was shared -> need to
            // check all other chunks to find the new border (can't just shrink
            // the asset aabb by the delta).
            RecomputeAABB(chunk_aabbs_, chunk_coords_, aabb_);
        } else {
            // The chunk grew beyond the old aabb border -> just expand the asset aabb.
            aabb_.Encapsulate(chunk_world_aabb);
        }
    } else {
        // Just incremental update.
        aabb_.Encapsulate(chunk_world_aabb);
    }
    BumpInstancesRevision();
    SetDirty();
    return h;
}

void GigaVoxel::UpdateChunk(GigaVoxelChunkId id,
                            GigaVoxelChunkCoord coord,
                            std::vector<GigaVoxelVertex> vertices,
                            std::vector<uint32_t> indices) {
    // UploadChunk already replaces an existing id, so delegate.
    UploadChunk(id, coord, std::move(vertices), std::move(indices));
}

void GigaVoxel::RemoveChunk(GigaVoxelChunkId id) {
    auto it = chunk_handles_.find(id);
    if (it == chunk_handles_.end()) return;
    GetGlobalGeometryHeap()->FreeChunk(it->second);
    chunk_handles_.erase(it);
    chunk_BLAS_.erase(id);
    // World AABB of the removed chunk (local AABB translated by its origin).
    AABB removed_world_aabb = AABB::Empty();
    auto lit = chunk_aabbs_.find(id);
    auto cit = chunk_coords_.find(id);
    if (lit != chunk_aabbs_.end() && cit != chunk_coords_.end()) {
        glm::vec3 origin = GigaVoxelChunkWorldOrigin(cit->second);
        removed_world_aabb = AABB(lit->second.min + origin, lit->second.max + origin);
    }
    chunk_aabbs_.erase(id);
    chunk_coords_.erase(id);
    blas_dirty_.erase(id);
    ReleaseChunkSlotAndPartition(id);
    {
        // If the removed chunk touched the asset AABB border, we need to check
        // all other chunks to find the new border
        auto min_aabb_border = glm::equal(removed_world_aabb.min, aabb_.min);
        auto max_aabb_border = glm::equal(removed_world_aabb.max, aabb_.max);
        if (glm::any(min_aabb_border) || glm::any(max_aabb_border)) {
            RecomputeAABB(chunk_aabbs_, chunk_coords_, aabb_);
        }
    }
    BumpInstancesRevision();
    SetDirty();
}

void GigaVoxel::ClearAllChunks() {
    for (auto & [id, h] : chunk_handles_) GetGlobalGeometryHeap()->FreeChunk(h);
    chunk_handles_.clear();
    chunk_BLAS_.clear();
    blas_dirty_.clear();
    chunk_aabbs_.clear();
    chunk_coords_.clear();
    std::vector<uint32_t> gathered_partitions;
    for (auto & [id, slot] : chunk_slots_) {
        (void)slot;
        auto pit = chunk_partitions_.find(id);
        if (pit != chunk_partitions_.end()) gathered_partitions.push_back(pit->second);
    }
    std::sort(gathered_partitions.begin(), gathered_partitions.end());
    gathered_partitions.erase(std::unique(gathered_partitions.begin(), gathered_partitions.end()), gathered_partitions.end());
    auto palloc = Renderer::Get().GetDeviceAllocator()->GetPartitionAllocator();
    for (auto e : gathered_partitions) {
        palloc->FreePartition(e);
    }
    chunk_slots_.clear();
    chunk_slots_allocator_.Reset();
    chunk_partitions_.clear();
    aabb_ = AABB::Empty();
    BumpInstancesRevision();
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
    auto cpu_v = GetGlobalGeometryHeap()->GetCPUVertices();
    auto cpu_i = GetGlobalGeometryHeap()->GetCPUIndices();
    for (const auto & [id, h] : chunk_handles_) {
        if (!h.valid || h.IsEmpty()) continue;
        Snap s;
        s.id = id;
        s.v.assign(cpu_v.begin() + h.vertex_offset, cpu_v.begin() + h.vertex_offset + h.vertex_count);
        s.i.assign(cpu_i.begin() + h.index_offset, cpu_i.begin() + h.index_offset + h.index_count);
        snaps.push_back(std::move(s));
    }

    // Wipe both heaps, the handle map, and all per-chunk BLASes (offsets moved).
    for (auto & [id, h] : chunk_handles_) GetGlobalGeometryHeap()->FreeChunk(h);
    chunk_handles_.clear();
    chunk_BLAS_.clear();
    blas_dirty_.clear();
    // Slots stay (chunk identity unchanged); partitions stay (still live).
    // Only the geometry moved, so just mark every re-allocated chunk BLAS dirty.

    // Re-allocate contiguously (no fragmentation). Upload geometry + chunk
    // header to GPU immediately so the moved data is valid before next use.
    // Slots, partitions, coords, and local AABBs all stay (only geometry moved).
    auto & compact_queue = RHI::Get().GetGraphicsCommandQueue();
    for (auto & s : snaps) {
        GigaVoxelChunkHandle h = GetGlobalGeometryHeap()->AllocateChunk(
            static_cast<uint32_t>(s.v.size()), static_cast<uint32_t>(s.i.size()));
        mi_assert(h.valid, "CompactChunks re-alloc failed (should always fit).");
        h.coord = chunk_coords_[s.id];  // preserve chunk identity -> world origin
        GetGlobalGeometryHeap()->UpdateChunk(h, s.v, s.i, &compact_queue);
        GetGlobalGeometryHeap()->UpdateChunkHeader(h, &compact_queue);
        chunk_handles_[s.id] = h;
        blas_dirty_.insert(s.id);  // new offset -> BLAS needs rebuild
    }
    // Compaction does not need to recompute AABB
    BumpInstancesRevision();
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
        size_t v_count = GetGlobalGeometryHeap()->GetVertexHighWatermark();
        size_t i_count = GetGlobalGeometryHeap()->GetIndexHighWatermark();
        GigaVoxelHeader header {};
        header.VertexOffset = 0;
        header.IndexOffset  = 0;
        header.VertexCount  = static_cast<uint32_t>(v_count);
        header.IndexCount   = static_cast<uint32_t>(i_count);
        header.AtlasBindlessIndex = GetGlobalAtlasBindlessIndex();
        Helpers::Upload_Async(queue, alloc->GetGigaVoxelHeaderBuffer(),
                              sizeof(GigaVoxelHeader) * device_giga_voxel_->GetIndex(), header);
    }

    // ---- Build per-chunk BLAS for every dirty chunk ----
    // Each chunk's BLAS references its OWN vertex/index span (offset+count from
    // the handle). Indices are chunk-local (0-based). The BLAS RHI object is
    // created lazily (CPU side, so the handle is valid immediately); build
    // commands are recorded for GPU execution.
    if (ray_traced_) {
        auto gpu_vbuf = GetGlobalGeometryHeap()->GetGPUVertexBuffer();
        auto gpu_ibuf = GetGlobalGeometryHeap()->GetGPUIndexBuffer();
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
    // Chunks may have gained/lost their BLAS this build -> bump the revision so
    // attached GigaVoxelInstances lazily rebuild their cached instance list and
    // TLAS gathering sees the up-to-date set.
    BumpInstancesRevision();
    SetDirty(false);
}

void GigaVoxel::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    BuildDirtyChunkBLAS_Async(alloc, queue);
    queue.WaitForIdle("GigaVoxel::UpdateOnDevice");
}

// ============================ GigaVoxelInstance =============================
GigaVoxelInstance::GigaVoxelInstance(Scene * scene)
    : Renderable(RenderableType::kGigaVoxelInstance, scene) {
    // Acquire a slot in the global GigaVoxelInstanceRTHeaderBuffer side table.
    // The high 8 bits of every per-chunk RT InstanceCustomIndex carry this slot
    // index so RT hit shaders can resolve the instance's RenderableIndex +
    // GigaVoxelIndex. Done in the ctor body (after Renderable base init) so
    // GetDeviceAllocator is reachable.
    auto * alloc = Renderer::Get().GetDeviceAllocator();
    rt_header_slot_ = alloc->AllocateGigaVoxelInstanceRTHeaderSlot();
}

GigaVoxelInstance::~GigaVoxelInstance() {
    // Release the RT header slot. GigaVoxelInstance destruction is routed through
    // the renderable delayed-free path (the scene keeps a TRef until the GPU has
    // finished consuming the last frame that referenced this instance), so by the
    // time we get here the GPU is no longer reading our slot row.
    if (rt_header_slot_ != 0xFFFFFFFFu) {
        Renderer::Get().GetDeviceAllocator()->FreeGigaVoxelInstanceRTHeaderSlot(rt_header_slot_);
        rt_header_slot_ = 0xFFFFFFFFu;
    }
}

TRef<GigaVoxelInstance> GigaVoxelInstance::Create(Scene * scene, TRef<GigaVoxel> giga_voxel, Transform transform) {
    if (!scene || !giga_voxel) return {};
    auto inst = TRef(new GigaVoxelInstance(scene));
    if (inst->IsValid()) {
        inst->SetTransform(transform);
        inst->scene_ = scene;
        inst->giga_voxel_ = std::move(giga_voxel);  // instance owns the asset
        return std::move(inst);
    }
    return {};
}

void GigaVoxelInstance::RebuildCachedInstances() {
    cached_instances_.clear();
    // Recompute this instance's world AABB as the union of per-chunk world
    // AABBs (chunk-local AABB translated by each chunk's world origin). This is
    // the culling bounds for the whole terrain instance.
    AABB instance_world_aabb = AABB::Empty();
    if (!giga_voxel_) {
        aabb_ = instance_world_aabb;
        return;
    }
    cached_instances_.reserve(giga_voxel_->chunk_handles_.size());
    // The per-chunk RT InstanceCustomIndex encodes:
    //   [rt_header_slot:8 bits 16-23][chunk_header_index:16 bits 0-15]
    // RT hit shaders decode the high 8 bits -> GigaVoxelInstanceRTHeaderBuffer
    // slot (owned by this instance, holds RenderableIndex + GigaVoxelIndex), and
    // the low 16 bits -> GigaVoxelChunkHeaderBuffer row (chunk geometry span).
    // See SharedGigaVoxel.hlsl (GigaVoxelInstanceRTHeader).
    const uint32_t rt_header_slot = rt_header_slot_;
    for (const auto & [id, h] : giga_voxel_->chunk_handles_) {
        auto bit = giga_voxel_->chunk_BLAS_.find(id);
        if (bit == giga_voxel_->chunk_BLAS_.end() || !bit->second) continue;  // BLAS not built yet
        auto pit = giga_voxel_->chunk_partitions_.find(id);
        auto cit = giga_voxel_->chunk_coords_.find(id);
        auto lit = giga_voxel_->chunk_aabbs_.find(id);
        glm::vec3 origin = (cit != giga_voxel_->chunk_coords_.end())
                               ? GigaVoxelChunkWorldOrigin(cit->second) : glm::vec3(0.0f);
        AABB local_aabb = (lit != giga_voxel_->chunk_aabbs_.end()) ? lit->second : GigaVoxelChunkLocalAABB();
        AABB world_aabb(local_aabb.min + origin, local_aabb.max + origin);
        instance_world_aabb.Encapsulate(world_aabb);

        RenderableBLASInstance inst {};
        inst.blas = bit->second.Raw();
        // Vertices are chunk-local: the per-chunk TLAS instance transform carries
        // the chunk's world placement. RT hit shaders read it via ObjectToWorld3x4().
        inst.transform = Transform{}.Translated(origin);
        // customIndex encodes: [rt_header_slot:8bits][chunk_header_index:16bits].
        // See the layout note at the top of this function.
        mi_assert(rt_header_slot < 256, "GigaVoxelInstanceRTHeader slot exceeds 8-bit encoding.");
        mi_assert(h.chunk_header_index < 65536, "chunk_header_index exceeds 16-bit encoding.");
        inst.instance_custom_index = (rt_header_slot << 16) | (h.chunk_header_index & 0xFFFFu);
        inst.instance_mask = 0xFF;
        inst.instance_contribution_to_hit_group_index =
            GigaVoxelInstance::kClassRegistrator.GetClassIndex();
        inst.partition_index = (pit != giga_voxel_->chunk_partitions_.end()) ? pit->second : 0;
        // PTLAS requires a world-space explicit_aabb.
        inst.explicit_aabb = world_aabb;
        cached_instances_.push_back(inst);
    }
    instances_revision_seen_ = giga_voxel_->instances_revision_;
    aabb_ = instance_world_aabb;
}

void GigaVoxelInstance::Update(RendererView * view, RenderGraphBuilder & builder) {
    // World AABB: the asset maintains it incrementally (union of per-chunk world
    // AABBs). RebuildCachedInstances also recomputes it; either source is fine.
    aabb_ = giga_voxel_ ? giga_voxel_->GetAABB() : AABB::Empty();

    // Build per-chunk BLAS for any dirty chunks. renderer touches RHI directly
    // (the queue buffers commands in submission order from the render thread),
    // so BLAS build commands land before the TLAS gather later this frame.
    auto * alloc = Renderer::Get().GetDeviceAllocator();
    if (giga_voxel_ && giga_voxel_->HasDirtyBLAS()) {
        auto & queue = RHI::Get().GetGraphicsCommandQueue();
        giga_voxel_->BuildDirtyChunkBLAS_Async(alloc, queue);
        SetBLASUpdated(true);
    }

    // Refresh this instance's row in the GigaVoxelInstanceRTHeaderBuffer side
    // table every frame. RT hit shaders index it via the high 8 bits of the
    // per-chunk InstanceCustomIndex to recover {RenderableIndex, GigaVoxelIndex}.
    // GigaVoxelIndex can change when device_giga_voxel_ is lazily created, so we
    // re-upload unconditionally (cheap: 8 bytes).
    if (alloc && rt_header_slot_ != 0xFFFFFFFFu) {
        GigaVoxelInstanceRTHeader row {};
        row.RenderableIndex = GetIndex();
        row.GigaVoxelIndex = (giga_voxel_ && giga_voxel_->GetDeviceGigaVoxel())
            ? giga_voxel_->GetDeviceGigaVoxel()->GetIndex() : 0xFFFFFFFFu;
        Helpers::Upload_Async(
            RHI::Get().GetGraphicsCommandQueue(),
            RHIBufferSpan{alloc->GetGigaVoxelInstanceRTHeaderBuffer(),
                          rt_header_slot_ * sizeof(GigaVoxelInstanceRTHeader),
                          sizeof(GigaVoxelInstanceRTHeader)},
            &row, sizeof(row));
    }
    SetDirty(false);
}

std::span<const RenderableBLASInstance> GigaVoxelInstance::GetPartitionedBLASInstances() const {
    if (!giga_voxel_) return {};
    // Lazily rebuild when the asset's instances_revision has advanced since the
    // last rebuild (chunk topology / BLAS change). Empty revision (UINT64_MAX)
    // forces the first rebuild.
    if (instances_revision_seen_ != giga_voxel_->instances_revision_) {
        const_cast<GigaVoxelInstance *>(this)->RebuildCachedInstances();
    }
    return std::span<const RenderableBLASInstance>(cached_instances_);
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
    // Only the legacy single-BLAS path uses this; the partitioned path encodes
    // [RTHeaderIndex:8][chunk_header_index:16] in RebuildCachedInstances. Return
    // the plain RenderableIndex here for consistency with the legacy contract.
    return GetIndex();
}

bool GigaVoxelInstance::IsEmpty() const {
    return !giga_voxel_ || giga_voxel_->IsEmpty();
}

MI_NAMESPACE_END
