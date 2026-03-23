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
#include "shaders/shared/SharedVolumePrimitives.hlsl"
#include "shaders/shared/SharedGaussianRadianceField.hlsl"
#include "shaders/shared/SharedVolumeGrid.hlsl"
#include "shaders/shared/SharedLightClusterHierarchy.hlsl"


MI_NAMESPACE_BEGIN

DeviceBindlessResourceAllocator::DeviceBindlessResourceAllocator():
material_slots_(kMaxNumMaterials),
geometry_slots_(kMaxNumGeometries),
static_mesh_slots_(kMaxNumStaticMeshes),
volume_primitives_slots_(kMaxNumVolumePrimitiveGroups),
gaussian_radiance_field_slots_(kMaxNumGaussianRadianceFields),
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

    mesh_light_cluster_header_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        alignof(MeshLightClusterHeader),
        16 * 1024,
        this
    );
    mesh_light_cluster_header_uber_buffer_->SetName("MeshLightClusterHeaderUberBuffer");

    mesh_light_cluster_node_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        alignof(MeshLightClusterNode),
        16 * 1024,
        this
    );
    mesh_light_cluster_node_uber_buffer_->SetName("MeshLightClusterNodeUberBuffer");

    mesh_light_instance_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        alignof(MeshLightInstance),
        16 * 1024,
        this
    );
    mesh_light_instance_uber_buffer_->SetName("MeshLightInstanceUberBuffer");

    volume_primitives_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(VolumePrimitivesHeader) * kMaxNumVolumePrimitiveGroups, RHIBufferUsageFlagBits::kStorage}
    );
    volume_primitives_header_buffer_->SetName("VolumePrimitivesHeaderBuffer");

    gaussian_radiance_field_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(GaussianRadianceFieldHeader) * kMaxNumGaussianRadianceFields, RHIBufferUsageFlagBits::kStorage}
    );
    gaussian_radiance_field_header_buffer_->SetName("GaussianRadianceFieldHeaderBuffer");

    volume_grid_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(VolumeGridHeader) * kMaxNumVolumeGrids, RHIBufferUsageFlagBits::kStorage}
    );
    volume_grid_header_buffer_->SetName("VolumeGridHeaderBuffer");
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
    sum += static_mesh_description_uber_buffer_->GetRHI()->GetBufferSize();
    sum += area_lights_uber_buffer_->GetRHI()->GetBufferSize();
    sum += mesh_light_cluster_header_uber_buffer_->GetRHI()->GetBufferSize();
    sum += mesh_light_cluster_node_uber_buffer_->GetRHI()->GetBufferSize();
    sum += mesh_light_instance_uber_buffer_->GetRHI()->GetBufferSize();
    sum += material_header_buffer_->GetBufferSize();
    sum += geometry_header_buffer_->GetBufferSize();
    sum += static_mesh_header_buffer_->GetBufferSize();
    sum += volume_primitives_header_buffer_->GetBufferSize();
    sum += gaussian_radiance_field_header_buffer_->GetBufferSize();
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
        case SlotKind::VolumePrimitives: idx = AllocateVolumePrimitivesSlot(); break;
        case SlotKind::VolumeGrid: idx = AllocateVolumeGridSlot(); break;
        case SlotKind::GaussianRadianceField: idx = AllocateGaussianRadianceFieldSlot(); break;
        default: break;
    }
    if (idx == UINT32_MAX) return {};

    // Bind a release function that calls back into FreeSlot with the right kind.
    struct Thunks {
        static void FreeMaterial(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::Material, v); }
        static void FreeGeometry(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::Geometry, v); }
        static void FreeStaticMesh(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::StaticMesh, v); }
        static void FreeVolumePrimitives(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::VolumePrimitives, v); }
        static void FreeVolumeGrid(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::VolumeGrid, v); }
        static void FreeGRF(DeviceBindlessResourceAllocator *o, uint32_t v) { o->FreeSlot(SlotKind::GaussianRadianceField, v); }
    };
    SlotKeeper::ReleaseFn fn = &ReleaseSlotThunk;
    switch (kind) {
        case SlotKind::Material: fn = &Thunks::FreeMaterial; break;
        case SlotKind::Geometry: fn = &Thunks::FreeGeometry; break;
        case SlotKind::StaticMesh: fn = &Thunks::FreeStaticMesh; break;
        case SlotKind::VolumePrimitives: fn = &Thunks::FreeVolumePrimitives; break;
        case SlotKind::VolumeGrid: fn = &Thunks::FreeVolumeGrid; break;
        case SlotKind::GaussianRadianceField: fn = &Thunks::FreeGRF; break;
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

void DeviceBindlessResourceAllocator::FreeSlot(SlotKind kind, uint32_t idx) {
    switch (kind) {
        case SlotKind::Material: FreeMaterialSlot(idx); break;
        case SlotKind::Geometry: FreeGeometrySlot(idx); break;
        case SlotKind::StaticMesh: FreeStaticMeshSlot(idx); break;
        case SlotKind::VolumePrimitives: FreeVolumePrimitivesSlot(idx); break;
        case SlotKind::VolumeGrid: FreeVolumeGridSlot(idx); break;
        case SlotKind::GaussianRadianceField: FreeGaussianRadianceFieldSlot(idx); break;
        default: break;
    }
}

MI_NAMESPACE_END
