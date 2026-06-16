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
    // Slot is freed (delayed) by the SlotKeeper. The BLAS is released by its
    // TRef destructor. Geometry lives in GigaVoxelGeometryHeap, not here.
}

// ================================ GigaVoxel =================================
GigaVoxel::GigaVoxel() {
    // GPU usage flags for the geometry heap: vertex/index + storage + AS build
    // input + shader device address (matches the old dedicated uber buffers).
    auto usage = RHIBufferUsageFlagBits::kVertex | RHIBufferUsageFlagBits::kIndex
               | RHIBufferUsageFlagBits::kStorage
               | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput
               | RHIBufferUsageFlagBits::kShaderDeviceAddress;
    geometry_heap_ = new GigaVoxelGeometryHeap(usage, usage);
}

TRef<GigaVoxel> GigaVoxel::Create() {
    return TRef(new GigaVoxel());
}

void GigaVoxel::SetDirty(bool dirty) {
    dirty_ = dirty;
    if (tracker_ && dirty_) tracker_->OnObjectTurnedDirty(this);
}

void GigaVoxel::SetAtlasTexture(TRef<Texture> atlas) {
    atlas_ = atlas;
    atlas_bindless_index_ = atlas_ ? atlas_->GetBindlessIndex() : 0xFFFFFFFFu;
    SetDirty();
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
    // Replace existing entry first (free old range).
    if (auto it = chunk_handles_.find(id); it != chunk_handles_.end()) {
        geometry_heap_->FreeChunk(it->second);
        chunk_handles_.erase(it);
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
    // The incoming indices are chunk-local (0-based, referencing this chunk's
    // own vertex range). The heap stores each chunk at vertex_offset, and the
    // (Phase-1) single BLAS is built over the whole heap at base 0, so the
    // BLAS-visible indices must be heap-global: rebias by vertex_offset.
    if (h.vertex_offset != 0) {
        for (auto & idx : indices) idx += h.vertex_offset;
    }
    geometry_heap_->UpdateChunk(h, vertices, indices);
    chunk_handles_[id] = h;
    RecomputeAABB(chunk_handles_, geometry_heap_.Raw(), aabb_);
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
    RecomputeAABB(chunk_handles_, geometry_heap_.Raw(), aabb_);
    SetDirty();
}

void GigaVoxel::ClearAllChunks() {
    for (auto & [id, h] : chunk_handles_) geometry_heap_->FreeChunk(h);
    chunk_handles_.clear();
    aabb_ = AABB::Empty();
    SetDirty();
}

void GigaVoxel::CompactChunks() {
    // Chunk-granular compaction: snapshot every live chunk's (rebased) geometry,
    // clear the heaps, then re-allocate contiguously in a stable order. This
    // keeps vertex/index offsets paired per chunk and re-applies the correct
    // heap-global index rebias, which a raw heap-level Compact could not do
    // (it doesn't know chunk grouping). O(chunks) — low-frequency fallback.
    if (chunk_handles_.empty()) return;

    // Snapshot {id, vertices, indices-global} in chunk-handle order.
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
        // De-bias back to chunk-local (undo the vertex_offset rebias) so the
        // re-upload path re-applies it consistently.
        s.i.reserve(h.index_count);
        for (uint32_t k = 0; k < h.index_count; ++k) {
            s.i.push_back(cpu_i[h.index_offset + k] - h.vertex_offset);
        }
        snaps.push_back(std::move(s));
    }

    // Wipe both heaps and the handle map.
    for (auto & [id, h] : chunk_handles_) geometry_heap_->FreeChunk(h);
    chunk_handles_.clear();

    // Re-allocate contiguously (no fragmentation). No queue here: GPU upload
    // is reconciled on the next UpdateOnDevice (full BLAS rebuild reads the
    // live watermark). Pass null queue to UpdateChunk for CPU-only writes.
    for (auto & s : snaps) {
        GigaVoxelChunkHandle h = geometry_heap_->AllocateChunk(
            static_cast<uint32_t>(s.v.size()), static_cast<uint32_t>(s.i.size()));
        mi_assert(h.valid, "CompactChunks re-alloc failed (should always fit).");
        if (h.vertex_offset != 0) {
            for (auto & idx : s.i) idx += h.vertex_offset;
        }
        geometry_heap_->UpdateChunk(h, s.v, s.i, /*queue*/ nullptr);
        chunk_handles_[s.id] = h;
    }
    RecomputeAABB(chunk_handles_, geometry_heap_.Raw(), aabb_);
    SetDirty();
}

void GigaVoxel::UpdateOnDevice_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    if (!dirty_) return;

    // Lazy device-object creation.
    if (!device_giga_voxel_) {
        device_giga_voxel_ = new DeviceGigaVoxel(alloc);
        mi_check(device_giga_voxel_->IsValid(), "Failed to allocate GigaVoxel slot.");
    }

    size_t v_count = geometry_heap_->GetVertexHighWatermark();
    size_t i_count = geometry_heap_->GetIndexHighWatermark();

    // ---- Write the GigaVoxelHeader into the header buffer ----
    // The heap base offset is 0; VertexCount/IndexCount bound the live range.
    GigaVoxelHeader header {};
    header.VertexOffset = 0;
    header.IndexOffset  = 0;
    header.VertexCount  = static_cast<uint32_t>(v_count);
    header.IndexCount   = static_cast<uint32_t>(i_count);
    header.AtlasBindlessIndex = atlas_bindless_index_;
    Helpers::Upload_Async(queue, alloc->GetGigaVoxelHeaderBuffer(),
                          sizeof(GigaVoxelHeader) * device_giga_voxel_->GetIndex(), header);

    // ---- Build / update the (Phase-1 single) BLAS over the live heap range ----
    bool has_geometry = !chunk_handles_.empty() && v_count > 0 && i_count > 0;
    if (!ray_traced_ || !has_geometry) {
        device_giga_voxel_->BLAS_ = {};
    } else {
        auto as_geom = queue.Allocate<RHIASGeometry>();
        RHIAccelerationStructureBuildFlags build_flags = RHIAccelerationStructureBuildFlagBits::kPreferFastTrace;
        *as_geom = RHIASGeometry{
            RHIASGeometryType::kTriangles,
            RHIASGeometryFlagBits::kOpaque, // VC geometry is opaque in Phase 1 (no transparent block pass yet)
            {
                RHIBufferSpan{geometry_heap_->GetGPUVertexBuffer(), 0, v_count * sizeof(GigaVoxelVertex)},
                sizeof(GigaVoxelVertex),
                static_cast<uint32_t>(v_count),
                RHIVertexAttributeFormatType::k3xFp32,
                RHIBufferSpan{geometry_heap_->GetGPUIndexBuffer(), 0, i_count * sizeof(uint32_t)},
                static_cast<uint32_t>(i_count),
                RHIIndexType::kUint32
            }
        };
        if (!device_giga_voxel_->BLAS_) {
            device_giga_voxel_->BLAS_ = RHI::Get().CreateAccelerationStructure(RHIAccelerationStructureType::kBottomLevel);
        }
        // Phase-1 skeleton: always rebuild over the live heap range.
        auto build_info = RHIAccelerationStructureBuildGeometryInfo{
            RHIAccelerationStructureType::kBottomLevel,
            build_flags,
            RHIAccelerationStructureBuildMode::kBuild,
            {},
            device_giga_voxel_->BLAS_.Raw(),
            {as_geom, 1}, {}, {}
        };
        auto sizes = device_giga_voxel_->BLAS_->GetBuildSizes(build_info);
        device_giga_voxel_->BLAS_->Create(sizes.acceleration_structure_size);
        auto scratch = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
        queue.BuildAccelerationStructure(build_info, scratch->GetSpan());
        queue.AccelerationStructureBarrier(
            device_giga_voxel_->BLAS_.Raw(),
            RHIPipelineStageFlagBits::kAccelerationStructureBuild,
            RHIPipelineStageFlagBits::kRayTracing | RHIPipelineStageFlagBits::kAccelerationStructureBuild,
            RHIGPUAccessFlagBits::kAccelerationStructureWrite,
            RHIGPUAccessFlagBits::kAccelerationStructureRW
        );
    }

    SetDirty(false);
}

void GigaVoxel::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    UpdateOnDevice_Async(alloc, queue);
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
        inst->giga_voxel_ = giga_voxel;
        return std::move(inst);
    }
    return {};
}

void GigaVoxelInstance::Update([[maybe_unused]] RendererView * view, [[maybe_unused]] RenderGraphBuilder & builder) {
    aabb_ = giga_voxel_ ? giga_voxel_->GetAABB() : AABB::Empty();
    SetDirty(false);
}

RenderableHeader GigaVoxelInstance::GetDeviceRenderableHeader() const {
    return std::bit_cast<RenderableHeader>(GigaVoxelInstanceHeader{
        giga_voxel_ && giga_voxel_->GetDeviceGigaVoxel() ? giga_voxel_->GetDeviceGigaVoxel()->GetIndex() : 0xFFFFFFFFu,
        0, 0, GetRenderableFlags()
    });
}

RHIAccelerationStructure * GigaVoxelInstance::GetBLAS() const {
    if (giga_voxel_ && giga_voxel_->IsRayTraced() && giga_voxel_->GetDeviceGigaVoxel()) {
        return giga_voxel_->GetDeviceGigaVoxel()->GetBLAS();
    }
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
