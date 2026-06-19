/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_resource_allocator.h"

#include <renderer/mi_material.h>
#include <rhi/rhi.h>

#include "renderer/mi_buffer_heap.h"
#include "rhi/rhi_buffer.h"
#include "rhi/rhi_bindless.h"
#include "shaders/shared/SharedMaterial.hlsl"
#include "shaders/shared/SharedVolumeGrid.hlsl"
#include "shaders/shared/SharedGigaVoxel.hlsl"
#include "shaders/shared/SharedLightClusterHierarchy.hlsl"


MI_NAMESPACE_BEGIN

DeviceBindlessResourceAllocator::DeviceBindlessResourceAllocator():
material_slots_(kMaxNumMaterials),
geometry_slots_(kMaxNumGeometries),
static_mesh_slots_(kMaxNumStaticMeshes),
giga_voxel_slots_(kMaxNumGigaVoxels),
volume_grid_slots_(kMaxNumVolumeGrids){

    vertex_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kVertex | RHIBufferUsageFlagBits::kStorage
        | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput | RHIBufferUsageFlagBits::kShaderDeviceAddress,
        128, 256 * 1024 * 1024, this
    );
    vertex_uber_buffer_->SetName("VertexUberBuffer");

    index_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kIndex | RHIBufferUsageFlagBits::kStorage
        | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput | RHIBufferUsageFlagBits::kShaderDeviceAddress,
        128, 256 * 1024 * 1024, this
    );
    index_uber_buffer_->SetName("IndexUberBuffer");

    // Dedicated GigaVoxel geometry heaps. Same usage flags as the StaticMesh
    // vertex/index heaps (vertex/index + storage + AS build input + device
    // address) but a separate allocator backing, sized smaller initially since
    // chunk geometry streams in progressively rather than loading all at once.
    giga_voxel_vertex_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kVertex | RHIBufferUsageFlagBits::kStorage
        | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput | RHIBufferUsageFlagBits::kShaderDeviceAddress,
        128, 64 * 1024 * 1024, this
    );
    giga_voxel_vertex_uber_buffer_->SetName("GigaVoxelVertexUberBuffer");

    giga_voxel_index_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kIndex | RHIBufferUsageFlagBits::kStorage
        | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput | RHIBufferUsageFlagBits::kShaderDeviceAddress,
        128, 64 * 1024 * 1024, this
    );
    giga_voxel_index_uber_buffer_->SetName("GigaVoxelIndexUberBuffer");

    material_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(MaterialHeader) * kMaxNumMaterials, RHIBufferUsageFlagBits::kStorage}
    );
    material_header_buffer_->SetName("MaterialHeaderBuffer");

    geometry_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(GeometryHeader) * kMaxNumGeometries, RHIBufferUsageFlagBits::kStorage}
    );
    geometry_header_buffer_->SetName("GeometryHeaderBuffer");

    static_mesh_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(StaticMeshHeader) * kMaxNumStaticMeshes, RHIBufferUsageFlagBits::kStorage}
    );
    static_mesh_header_buffer_->SetName("StaticMeshHeaderBuffer");

    // History transform buffer for motion vectors (float3x4 per renderable)
    prev_renderable_transform_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(glm::mat4x3) * Scene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kTransferSrc}
    );
    prev_renderable_transform_buffer_->SetName("PrevRenderableTransforms");

    renderable_hash_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(uint32_t) * Scene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kTransferSrc}
    );
    renderable_hash_buffer_->SetName("RenderableHashBuffer");

    prev_renderable_hash_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(uint32_t) * Scene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kTransferSrc}
    );
    prev_renderable_hash_buffer_->SetName("PrevRenderableHashBuffer");

    static_mesh_description_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        1, 16 * 1024, this
    );
    static_mesh_description_uber_buffer_->SetName("StaticMeshDescriptionUberBuffer");

    area_lights_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        1, 16 * 1024, this
    );
    area_lights_uber_buffer_->SetName("AreaLightsUberBuffer");

    mesh_light_triangle_uber_buffer_array_ = DefaultDeviceUberBufferArray::Create(
        {
            {RHIBufferUsageFlagBits::kStorage, sizeof(MeshLightTriangle), "MeshLightTriangleUberBuffer"},
            {RHIBufferUsageFlagBits::kStorage, sizeof(MeshLightTriangleHash), "MeshLightTriangleHashUberBuffer"},
            {RHIBufferUsageFlagBits::kStorage, sizeof(MeshLightTriangleBakedData), "MeshLightTriangleBakedDataUberBuffer"}
        },
        1,
        16 * 1024,
        this
    );
    mesh_light_triangle_uber_buffer_array_->SetName("MeshLightTriangleStreams");

    mesh_light_cluster_uber_buffer_array_ = DefaultDeviceUberBufferArray::Create(
        {
            {RHIBufferUsageFlagBits::kStorage, sizeof(MeshLightClusterHeader), "MeshLightClusterHeaderUberBuffer"},
            {RHIBufferUsageFlagBits::kStorage, sizeof(MeshLightClusterNode), "MeshLightClusterNodeUberBuffer"}
        },
        1,
        16 * 1024,
        this
    );
    mesh_light_cluster_uber_buffer_array_->SetName("MeshLightClusterStreams");

    mesh_light_level_header_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        alignof(MeshLightLevelHeader),
        16 * 1024,
        this
    );
    mesh_light_level_header_uber_buffer_->SetName("MeshLightLevelHeaderUberBuffer");

    mesh_light_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        alignof(MeshLight),
        16 * 1024,
        this
    );
    mesh_light_uber_buffer_->SetName("MeshLightUberBuffer");

    mesh_light_instance_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        alignof(MeshLightInstance),
        16 * 1024,
        this
    );
    mesh_light_instance_uber_buffer_->SetName("MeshLightInstanceUberBuffer");

    mesh_light_instance_cluster_uber_buffer_array_ = DefaultDeviceUberBufferArray::Create(
        {
            {RHIBufferUsageFlagBits::kStorage, sizeof(MeshLightInstanceClusterHeader), "MeshLightInstanceClusterHeaderUberBuffer"},
            {RHIBufferUsageFlagBits::kStorage, sizeof(MeshLightInstanceClusterNode), "MeshLightInstanceClusterNodeUberBuffer"}
        },
        1,
        16 * 1024,
        this
    );
    mesh_light_instance_cluster_uber_buffer_array_->SetName("MeshLightInstanceClusterStreams");

    mesh_light_instance_triangle_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        alignof(MeshLightInstanceTriangle),
        16 * 1024,
        this
    );
    mesh_light_instance_triangle_uber_buffer_->SetName("MeshLightInstanceTriangleUberBuffer");

    volume_grid_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(VolumeGridHeader) * kMaxNumVolumeGrids, RHIBufferUsageFlagBits::kStorage}
    );
    volume_grid_header_buffer_->SetName("VolumeGridHeaderBuffer");

    giga_voxel_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(GigaVoxelHeader) * kMaxNumGigaVoxels, RHIBufferUsageFlagBits::kStorage}
    );
    giga_voxel_header_buffer_->SetName("GigaVoxelHeaderBuffer");
}

DeviceBindlessResourceAllocator::~DeviceBindlessResourceAllocator() {
    // Drop anything still pending.
    delayed_destruction_.ClearAllNow();
}

void DeviceBindlessResourceAllocator::AdvanceFrame() {
    delayed_destruction_.Tick();
}

void DeviceBindlessResourceAllocator::EnqueueForDelayedDestruction(DelayedDestructionResource * obj) {
    delayed_destruction_.Enqueue(obj);
}

void DeviceBindlessResourceAllocator::AdvanceFrameForDelayedDestruction() {
    delayed_destruction_.Tick();
}

void DeviceBindlessResourceAllocator::ForceFlushDelayedDestruction() {
    delayed_destruction_.ClearAllNow();
}

size_t DeviceBindlessResourceAllocator::GetTotalAllocatedDeviceSize() const {
    size_t sum = 0;
    sum += vertex_uber_buffer_->GetRHI()->GetBufferSize();
    sum += index_uber_buffer_->GetRHI()->GetBufferSize();
    sum += giga_voxel_vertex_uber_buffer_->GetRHI()->GetBufferSize();
    sum += giga_voxel_index_uber_buffer_->GetRHI()->GetBufferSize();
    sum += static_mesh_description_uber_buffer_->GetRHI()->GetBufferSize();
    sum += area_lights_uber_buffer_->GetRHI()->GetBufferSize();
    for (uint32_t i = 0; i < mesh_light_triangle_uber_buffer_array_->GetNumStreams(); ++i) {
        sum += mesh_light_triangle_uber_buffer_array_->GetRHI(i)->GetBufferSize();
    }
    for (uint32_t i = 0; i < mesh_light_cluster_uber_buffer_array_->GetNumStreams(); ++i) {
        sum += mesh_light_cluster_uber_buffer_array_->GetRHI(i)->GetBufferSize();
    }
    sum += mesh_light_level_header_uber_buffer_->GetRHI()->GetBufferSize();
    sum += mesh_light_uber_buffer_->GetRHI()->GetBufferSize();
    sum += mesh_light_instance_uber_buffer_->GetRHI()->GetBufferSize();
    for (uint32_t i = 0; i < mesh_light_instance_cluster_uber_buffer_array_->GetNumStreams(); ++i) {
        sum += mesh_light_instance_cluster_uber_buffer_array_->GetRHI(i)->GetBufferSize();
    }
    sum += mesh_light_instance_triangle_uber_buffer_->GetRHI()->GetBufferSize();
    sum += material_header_buffer_->GetBufferSize();
    sum += geometry_header_buffer_->GetBufferSize();
    sum += static_mesh_header_buffer_->GetBufferSize();
    sum += giga_voxel_header_buffer_->GetBufferSize();
    return sum;
}

static void ReleaseSlotThunk(DeviceBindlessResourceAllocator *owner, uint32_t value) {
    // Default thunk is unused; we bind per-kind thunks below.
    (void)owner; (void)value;
}

TRef<DeviceBindlessResourceAllocator::SlotKeeper> DeviceBindlessResourceAllocator::AllocateSlotKeeper(SlotKind kind) {
    uint32_t idx = UINT32_MAX;
    switch (kind) {
        case SlotKind::Material: idx = AllocateMaterialSlot(); break;
        case SlotKind::Geometry: idx = AllocateGeometrySlot(); break;
        case SlotKind::StaticMesh: idx = AllocateStaticMeshSlot(); break;
        case SlotKind::VolumeGrid: idx = AllocateVolumeGridSlot(); break;
        case SlotKind::GigaVoxel: idx = AllocateGigaVoxelSlot(); break;
        default: break;
    }
    if (idx == UINT32_MAX) return {};

    // Bind a release function that calls back into FreeSlot with the right kind.
    struct Thunks {
        static void FreeMaterial(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::Material, v); }
        static void FreeGeometry(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::Geometry, v); }
        static void FreeStaticMesh(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::StaticMesh, v); }
        static void FreeVolumeGrid(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::VolumeGrid, v); }
        static void FreeGigaVoxel(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::GigaVoxel, v); }
    };
    SlotKeeper::ReleaseFn fn = &ReleaseSlotThunk;
    switch (kind) {
        case SlotKind::Material: fn = &Thunks::FreeMaterial; break;
        case SlotKind::Geometry: fn = &Thunks::FreeGeometry; break;
        case SlotKind::StaticMesh: fn = &Thunks::FreeStaticMesh; break;
        case SlotKind::VolumeGrid: fn = &Thunks::FreeVolumeGrid; break;
        case SlotKind::GigaVoxel: fn = &Thunks::FreeGigaVoxel; break;
        default: break;
    }

    return TRef<SlotKeeper>(new SlotKeeper(this, idx, fn));
}

void DeviceBindlessResourceAllocator::RegisterCustomBufferHeap (uint32_t index, DeviceBufferHeapInterface * heap) {
    assert(custom_buffer_heaps_.find(index) == custom_buffer_heaps_.end() && "Custom buffer heap already registered for this index.");
    custom_buffer_heaps_[index] = heap;
}

void DeviceBindlessResourceAllocator::RegisterCustomUberBuffer (uint32_t index, DeviceUberBufferInterface * uber_buffer) {
    assert(custom_uber_buffers_.find(index) == custom_uber_buffers_.end() && "Custom uber buffer already registered for this index.");
    custom_uber_buffers_[index] = uber_buffer;
}

std::pair<TRef<DeviceUberBufferAllocation>, bool> DeviceBindlessResourceAllocator::AllocateVertexBuffer (uint32_t size, bool allow_reallocation) {
    return vertex_uber_buffer_->AllocateRefCounted(size, allow_reallocation);
}
std::pair<TRef<DeviceUberBufferAllocation>, bool> DeviceBindlessResourceAllocator::AllocateIndexBuffer (uint32_t size, bool allow_reallocation) {
    return index_uber_buffer_->AllocateRefCounted(size, allow_reallocation);
}
std::pair<TRef<DeviceUberBufferAllocation>, bool> DeviceBindlessResourceAllocator::AllocateGigaVoxelVertexBuffer (uint32_t size, bool allow_reallocation) {
    return giga_voxel_vertex_uber_buffer_->AllocateRefCounted(size, allow_reallocation);
}
std::pair<TRef<DeviceUberBufferAllocation>, bool> DeviceBindlessResourceAllocator::AllocateGigaVoxelIndexBuffer (uint32_t size, bool allow_reallocation) {
    return giga_voxel_index_uber_buffer_->AllocateRefCounted(size, allow_reallocation);
}

void DeviceBindlessResourceAllocator::FreeSlot(SlotKind kind, uint32_t idx) {
    switch (kind) {
        case SlotKind::Material: FreeMaterialSlot(idx); break;
        case SlotKind::Geometry: FreeGeometrySlot(idx); break;
        case SlotKind::StaticMesh: FreeStaticMeshSlot(idx); break;
        case SlotKind::VolumeGrid: FreeVolumeGridSlot(idx); break;
        case SlotKind::GigaVoxel: FreeGigaVoxelSlot(idx); break;
        default: break;
    }
}

MI_NAMESPACE_END
