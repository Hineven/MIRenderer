/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RESOURCE_ALLOCATOR_H
#define MI_RESOURCE_ALLOCATOR_H

#include <map>
#include <mutex>
#include <set>
#include <stack>
#include <vector>

#include <core/util/slot_allocator.h>
#include <core/base.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>

#include <renderer/mi_delayed_destruction.h>
#include <renderer/mi_scene.h>
#include <renderer/mi_resource_allocator_slot.h>

#include "mi_partition_allocator.h"
#include "../shaders/shared/SharedStaticMesh.hlsl"
#include "../shaders/shared/SharedGigaVoxel.hlsl"

MI_NAMESPACE_BEGIN

// Forward declarations (defined in their own headers; kept here to avoid pulling
// heavy RHI includes into this widely-included header). The full definitions are
// visible in the .cpp where TRef<GigaVoxelGeometryHeap> is constructed/destroyed.
class GigaVoxelGeometryHeap;
class Texture;

// Allocate GPU resources used for common bindless rendering (device geometries, materials, static meshes, etc)
// Resources that does not need to be bindless, or already bindless via RHI layers (for example, textures) should
// be allocated separately.
// One allocator for one renderer.
class DeviceBindlessResourceAllocator : public NonCopyable, public NonMovable, public RefCounted<>, public IDeferredFreeOwner {
public:
    DeviceBindlessResourceAllocator ();
    ~DeviceBindlessResourceAllocator();

    friend class Renderer;

    // Typed bindless slot categories. Public so keepers can be used by other systems too.
    enum class SlotKind : uint8_t {
        Material,
        Geometry,
        StaticMesh,
        VolumeGrid,
        GigaVoxel,
    };

    // Keeper type for bindless slots.
    using SlotKeeper = DeviceBindlessResourceSlotKeeper;

    // Allocate a keeper that will release the slot a few frames after the last reference is dropped.
    TRef<SlotKeeper> AllocateSlotKeeper(SlotKind kind);

    FORCEINLINE TRef<SlotKeeper> AllocateMaterialSlotKeeper() { return AllocateSlotKeeper(SlotKind::Material); }
    FORCEINLINE TRef<SlotKeeper> AllocateGeometrySlotKeeper() { return AllocateSlotKeeper(SlotKind::Geometry); }
    FORCEINLINE TRef<SlotKeeper> AllocateStaticMeshSlotKeeper() { return AllocateSlotKeeper(SlotKind::StaticMesh); }
    FORCEINLINE TRef<SlotKeeper> AllocateVolumeGridSlotKeeper() { return AllocateSlotKeeper(SlotKind::VolumeGrid); }
    FORCEINLINE TRef<SlotKeeper> AllocateGigaVoxelSlotKeeper() { return AllocateSlotKeeper(SlotKind::GigaVoxel); }

    // Unified free by kind (used by keepers). This is the actual slot recycle.
    // Do not call this directly unless you know exactly what you are doing.
    void FreeSlot(SlotKind kind, uint32_t idx);

    static constexpr uint32_t kMaxNumMaterials = 1024;
    static constexpr uint32_t kMaxNumGeometries = 64 * 1024; // 64K geometries
    static constexpr uint32_t kMaxNumStaticMeshes = 64 * 1024;
    static constexpr uint32_t kMaxNumVolumeGrids = 256;
    static constexpr uint32_t kMaxNumGigaVoxels = 256; // A scene has only a handful of GigaVoxel terrains
    static constexpr uint32_t kMaxNumGigaVoxelInstances = 256; // Per-instance RT header side table size (GigaVoxelInstanceRTHeaderBuffer)

    FORCEINLINE DeviceUberBufferInterface * GetVertexUberBuffer () const {
        return vertex_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetIndexUberBuffer () const {
        return index_uber_buffer_.Raw();
    }

    // Dedicated GigaVoxel vertex/index uber buffers.
    // DEPRECATED: GigaVoxel geometry is now managed by GigaVoxelGeometryHeap
    // (size-class allocator), which owns its own dedicated RHIBuffers. These
    // uber buffers and their Allocate methods are no longer used by the
    // GigaVoxel path and will be removed in a follow-up cleanup. Kept for now
    // to avoid breaking any external references.
    [[deprecated("GigaVoxel now uses GigaVoxelGeometryHeap; see mi_giga_voxel_heap.h")]]
    FORCEINLINE DeviceUberBufferInterface * GetGigaVoxelVertexUberBuffer () const {
        return giga_voxel_vertex_uber_buffer_.Raw();
    }
    [[deprecated("GigaVoxel now uses GigaVoxelGeometryHeap; see mi_giga_voxel_heap.h")]]
    FORCEINLINE DeviceUberBufferInterface * GetGigaVoxelIndexUberBuffer () const {
        return giga_voxel_index_uber_buffer_.Raw();
    }

    void RegisterCustomBufferHeap (uint32_t index, DeviceBufferHeapInterface * heap) ;

    FORCEINLINE DeviceBufferHeapInterface * GetCustomBufferHeap (uint32_t index) const {
        return custom_buffer_heaps_.at(index).Raw();
    }

    void RegisterCustomUberBuffer (uint32_t index, DeviceUberBufferInterface * uber_buffer) ;

    FORCEINLINE DeviceUberBufferInterface * GetCustomUberBuffer (uint32_t index) const {
        auto it = custom_uber_buffers_.find(index);
        if (it != custom_uber_buffers_.end()) {
            return it->second.Raw();
        }
        return nullptr;
    }

    // Allocate a vertex buffer from the vertex buffer heap.
    std::pair<TRef<DeviceUberBufferAllocation>, bool> AllocateVertexBuffer (uint32_t size, bool allow_reallocation = true) ;
    std::pair<TRef<DeviceUberBufferAllocation>, bool> AllocateIndexBuffer (uint32_t size, bool allow_reallocation = true) ;
    // Allocate from the dedicated GigaVoxel geometry heaps.
    // DEPRECATED: GigaVoxel now uses GigaVoxelGeometryHeap. See note above.
    [[deprecated("GigaVoxel now uses GigaVoxelGeometryHeap")]]
    std::pair<TRef<DeviceUberBufferAllocation>, bool> AllocateGigaVoxelVertexBuffer (uint32_t size, bool allow_reallocation = true) ;
    [[deprecated("GigaVoxel now uses GigaVoxelGeometryHeap")]]
    std::pair<TRef<DeviceUberBufferAllocation>, bool> AllocateGigaVoxelIndexBuffer (uint32_t size, bool allow_reallocation = true) ;

    FORCEINLINE uint32_t AllocateMaterialSlot () {
        return material_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeMaterialSlot (uint32_t idx) {
        assert(idx < kMaxNumMaterials);
        material_slots_.FreeSlot(idx);
    }

    FORCEINLINE uint32_t AllocateGeometrySlot () {
        return geometry_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeGeometrySlot (uint32_t idx) {
        assert(idx < kMaxNumGeometries);
        geometry_slots_.FreeSlot(idx);
    }

    FORCEINLINE uint32_t AllocateStaticMeshSlot () {
        return static_mesh_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeStaticMeshSlot (uint32_t idx) {
        assert(idx < kMaxNumStaticMeshes);
        static_mesh_slots_.FreeSlot(idx);
    }

    FORCEINLINE uint32_t AllocateVolumeGridSlot () {
        return volume_grid_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeVolumeGridSlot (uint32_t idx) {
        assert(idx < kMaxNumVolumeGrids);
        volume_grid_slots_.FreeSlot(idx);
    }

    FORCEINLINE uint32_t AllocateGigaVoxelSlot () {
        return giga_voxel_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeGigaVoxelSlot (uint32_t idx) {
        assert(idx < kMaxNumGigaVoxels);
        giga_voxel_slots_.FreeSlot(idx);
    }

    FORCEINLINE RHIBuffer * GetStaticMeshHeaderBuffer() const {
        return static_mesh_header_buffer_.Raw();
    }
    FORCEINLINE DeviceUberBufferInterface * GetStaticMeshDescriptionUberBuffer() const {
        return static_mesh_description_uber_buffer_.Raw();
    }

    FORCEINLINE RHIBuffer * GetMaterialHeaderBuffer() const {
        return material_header_buffer_.Raw();
    }
    FORCEINLINE RHIBuffer * GetGeometryHeaderBuffer() const {
        return geometry_header_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetAreaLightsUberBuffer() const {
        return area_lights_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferArrayInterface * GetMeshLightTriangleUberBufferArray() const {
        return mesh_light_triangle_uber_buffer_array_.Raw();
    }

    FORCEINLINE DeviceUberBufferArrayInterface * GetMeshLightClusterUberBufferArray() const {
        return mesh_light_cluster_uber_buffer_array_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetMeshLightLevelHeaderUberBuffer() const {
        return mesh_light_level_header_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetMeshLightUberBuffer() const {
        return mesh_light_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetMeshLightInstanceUberBuffer() const {
        return mesh_light_instance_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferArrayInterface * GetMeshLightInstanceClusterUberBufferArray() const {
        return mesh_light_instance_cluster_uber_buffer_array_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetMeshLightInstanceTriangleUberBuffer() const {
        return mesh_light_instance_triangle_uber_buffer_.Raw();
    }

    FORCEINLINE RHIBuffer * GetVolumeGridHeaderBuffer() const {
        return volume_grid_header_buffer_.Raw();
    }
    FORCEINLINE RHIBuffer * GetGigaVoxelHeaderBuffer() const {
        return giga_voxel_header_buffer_.Raw();
    }

    // ---- GigaVoxel per-instance RT header side table ----
    // GigaVoxel packs [RTHeaderIndex:8][chunk_header_index:16] into InstanceCustomIndex;
    // the high 8 bits index this fixed-size table. Each GigaVoxelInstance acquires
    // one slot on construction and releases it (delayed) on destruction. The slot
    // row stores {RenderableIndex, GigaVoxelIndex} so RT hit shaders can reach both
    // scene-level per-renderable buffers and the asset-level GigaVoxelHeaderBuffer.
    // See SharedGigaVoxel.hlsl (GigaVoxelInstanceRTHeader) for the row layout.
    uint32_t AllocateGigaVoxelInstanceRTHeaderSlot();
    void FreeGigaVoxelInstanceRTHeaderSlot(uint32_t idx);
    FORCEINLINE RHIBuffer * GetGigaVoxelInstanceRTHeaderBuffer() const {
        return giga_voxel_instance_rt_header_buffer_.Raw();
    }

    // ---- Global GigaVoxel geometry heap + block atlas ----
    // These are process-global GigaVoxel resources that previously lived as
    // class-statics on GigaVoxel (mi_giga_voxel.h). Owning them here guarantees
    // they are destroyed before the RHI singleton (the allocator is torn down in
    // the app shutdown sequence before RHI::DestroySingleton), fixing the
    // static-destruction-order crash on exit. The GigaVoxel::GetGlobalGeometryHeap()
    // / SetGlobalAtlas() static accessors delegate to these.
    // Lazy-created on first access.
    GigaVoxelGeometryHeap * GetGigaVoxelGeometryHeap();
    void SetGigaVoxelAtlas(TRef<Texture> atlas);
    FORCEINLINE uint32_t GetGigaVoxelAtlasBindlessIndex() const {
        return giga_voxel_atlas_bindless_index_;
    }

    FORCEINLINE RHIBuffer * GetPrevRenderableTransformBuffer() const {
        return prev_renderable_transform_buffer_.Raw();
    }
    FORCEINLINE RHIBuffer * GetRenderableHashBuffer() const {
        return renderable_hash_buffer_.Raw();
    }
    FORCEINLINE RHIBuffer * GetPrevRenderableHashBuffer() const {
        return prev_renderable_hash_buffer_.Raw();
    }

    size_t GetTotalAllocatedDeviceSize () const ;

    // Centralized delayed destruction:
    // Some renderer-side resources are not RHIResource (thus not protected by RHI deferred deletion)
    // but still must not be destroyed while the GPU may reference them (e.g. uber-buffer sub-allocations,
    // slot allocations mirrored into persistent device buffers, etc.).
    //
    // The owning system should call AdvanceFrame() once per frame at a point where frame N-1 has finished on GPU.
    void AdvanceFrame();

    // Enqueue an arbitrary delayed-destruction object.
    // The queue takes ownership of the pointer and will delete it after a few frames.
    void EnqueueForDelayedDestruction(DelayedDestructionResource * obj) override;
    // Advance frame, removing objects whose delay has elapsed.
    void AdvanceFrameForDelayedDestruction() override;
    // Force-delete all delayed destruction objects immediately.
    // Intended for shutdown path where no more frames will be advanced.
    void ForceFlushDelayedDestruction();

    PartitionAllocator * GetPartitionAllocator();

protected:
    // Underlying buffer holding the material headers. This is updated on a per-frame basis.
    // Allocated a proper size upon construction.
    TRef<RHIBuffer> material_header_buffer_;
    // History buffer of renderable transforms (float3x4 per renderable)
    TRef<RHIBuffer> prev_renderable_transform_buffer_;
    // Hash buffers for renderables
    TRef<RHIBuffer> renderable_hash_buffer_;
    TRef<RHIBuffer> prev_renderable_hash_buffer_;

    // Uber buffers for consistent geometries
    TRef<DeviceUberBufferInterface> vertex_uber_buffer_;
    TRef<DeviceUberBufferInterface> index_uber_buffer_;
    // Dedicated GigaVoxel geometry heaps (see GetGigaVoxelVertexUberBuffer comment).
    TRef<DeviceUberBufferInterface> giga_voxel_vertex_uber_buffer_;
    TRef<DeviceUberBufferInterface> giga_voxel_index_uber_buffer_;
    // Header for geometries.
    // A geometry header holds DeviceGeometryHeader structs.
    TRef<RHIBuffer> geometry_header_buffer_;
    // A static mesh header buffer holding StaticMeshHeader structs.
    TRef<RHIBuffer> static_mesh_header_buffer_;
    // A static mesh description heap (single block buffer heap), use the offsets in static mesh header to access the descriptions.
    TRef<DeviceUberBufferInterface> static_mesh_description_uber_buffer_;
    // A buffer holding the volume grid headers. (VolumeGridHeader)
    TRef<RHIBuffer> volume_grid_header_buffer_;
    // A buffer holding the GigaVoxel headers. (GigaVoxelHeader)
    TRef<RHIBuffer> giga_voxel_header_buffer_;
    // Per-instance RT header side table for GigaVoxel (GigaVoxelInstanceRTHeader rows,
    // indexed by the high 8 bits of a GigaVoxel RT InstanceCustomIndex).
    TRef<RHIBuffer> giga_voxel_instance_rt_header_buffer_;

    // Global GigaVoxel geometry heap (shared vertex/index/chunk-header GPU buffers
    // for all GigaVoxel assets) + global block atlas. Owned here so they die before
    // the RHI singleton (see GetGigaVoxelGeometryHeap() comment above).
    TRef<GigaVoxelGeometryHeap> giga_voxel_geometry_heap_;
    TRef<Texture> giga_voxel_atlas_;
    uint32_t giga_voxel_atlas_bindless_index_ = 0xFFFFFFFFu;

    // A buffer holding all area lights (RawLight structs).
    TRef<DeviceUberBufferInterface> area_lights_uber_buffer_;

    // Mesh light hierarchy buffers.
    TRef<DeviceUberBufferArrayInterface> mesh_light_triangle_uber_buffer_array_;
    TRef<DeviceUberBufferArrayInterface> mesh_light_cluster_uber_buffer_array_;
    TRef<DeviceUberBufferInterface> mesh_light_level_header_uber_buffer_;
    TRef<DeviceUberBufferInterface> mesh_light_uber_buffer_;
    TRef<DeviceUberBufferInterface> mesh_light_instance_uber_buffer_;
    TRef<DeviceUberBufferArrayInterface> mesh_light_instance_cluster_uber_buffer_array_;
    TRef<DeviceUberBufferInterface> mesh_light_instance_triangle_uber_buffer_;

    // Custom buffer heaps for custom resources (e.g. custom renderable class)
    std::map<uint32_t, TRef<DeviceBufferHeapInterface>> custom_buffer_heaps_;
    // Custom uber buffers for custom resources (e.g. custom renderable class)
    std::map<uint32_t, TRef<DeviceUberBufferInterface>> custom_uber_buffers_;

    // Slot allocators for bindless resources
    SlotAllocator material_slots_, geometry_slots_, static_mesh_slots_, volume_grid_slots_, giga_voxel_slots_;
    // Slot allocator for the GigaVoxel per-instance RT header side table.
    SlotAllocator giga_voxel_instance_rt_header_slots_ {kMaxNumGigaVoxelInstances};

    // Partiton allocator for PTLAS
    TRef<PartitionAllocator> partition_allocator_;

    // Central delayed destruction ring.
    // Stores resources whose refcount already reached 0 and are safe to delete after N frames.
    DelayedDestructionQueue delayed_destruction_ {2};
};

MI_NAMESPACE_END

#endif //MI_RESOURCE_ALLOCATOR_H
