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
#include "../shaders/shared/SharedStaticMesh.hlsl"

MI_NAMESPACE_BEGIN

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
        VolumePrimitives,
        VolumeGrid,
        GaussianRadianceField,
    };

    // Keeper type for bindless slots.
    using SlotKeeper = DeviceBindlessResourceSlotKeeper;

    // Allocate a keeper that will release the slot a few frames after the last reference is dropped.
    TRef<SlotKeeper> AllocateSlotKeeper(SlotKind kind);

    FORCEINLINE TRef<SlotKeeper> AllocateMaterialSlotKeeper() { return AllocateSlotKeeper(SlotKind::Material); }
    FORCEINLINE TRef<SlotKeeper> AllocateGeometrySlotKeeper() { return AllocateSlotKeeper(SlotKind::Geometry); }
    FORCEINLINE TRef<SlotKeeper> AllocateStaticMeshSlotKeeper() { return AllocateSlotKeeper(SlotKind::StaticMesh); }
    FORCEINLINE TRef<SlotKeeper> AllocateVolumePrimitivesSlotKeeper() { return AllocateSlotKeeper(SlotKind::VolumePrimitives); }
    FORCEINLINE TRef<SlotKeeper> AllocateVolumeGridSlotKeeper() { return AllocateSlotKeeper(SlotKind::VolumeGrid); }
    FORCEINLINE TRef<SlotKeeper> AllocateGaussianRadianceFieldSlotKeeper() { return AllocateSlotKeeper(SlotKind::GaussianRadianceField); }

    // Unified free by kind (used by keepers). This is the actual slot recycle.
    // Do not call this directly unless you know exactly what you are doing.
    void FreeSlot(SlotKind kind, uint32_t idx);

    static constexpr uint32_t kMaxNumMaterials = 1024;
    static constexpr uint32_t kMaxNumGeometries = 64 * 1024; // 64K geometries
    static constexpr uint32_t kMaxNumStaticMeshes = 64 * 1024;
    // static constexpr uint32_t kMaxNumStaticMeshGeometryMaterialPairs = 256 * 1024;
    static constexpr uint32_t kMaxNumVolumePrimitiveGroups = 1024; // 1K volume primitive groups (assume that there are not many)
    static constexpr uint32_t kMaxNumGaussianRadianceFields = 256; // Assume fewer GRF datasets
    static constexpr uint32_t kMaxNumVolumeGrids = 256;

    FORCEINLINE DeviceUberBufferInterface * GetVertexUberBuffer () const {
        return vertex_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetIndexUberBuffer () const {
        return index_uber_buffer_.Raw();
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
        assert(idx < kMaxNumMaterials);
        geometry_slots_.FreeSlot(idx);
    }

    FORCEINLINE uint32_t AllocateStaticMeshSlot () {
        return static_mesh_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeStaticMeshSlot (uint32_t idx) {
        assert(idx < kMaxNumMaterials);
        static_mesh_slots_.FreeSlot(idx);
    }

    FORCEINLINE uint32_t AllocateVolumePrimitivesSlot () {
        return volume_primitives_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeVolumePrimitivesSlot (uint32_t idx) {
        assert(idx < kMaxNumMaterials);
        volume_primitives_slots_.FreeSlot(idx);
    }

    FORCEINLINE uint32_t AllocateVolumeGridSlot () {
        return volume_grid_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeVolumeGridSlot (uint32_t idx) {
        assert(idx < kMaxNumMaterials);
        volume_grid_slots_.FreeSlot(idx);
    }

    FORCEINLINE uint32_t AllocateGaussianRadianceFieldSlot () {
        return gaussian_radiance_field_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeGaussianRadianceFieldSlot (uint32_t idx) {
        assert(idx < kMaxNumGaussianRadianceFields);
        gaussian_radiance_field_slots_.FreeSlot(idx);
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

    FORCEINLINE DeviceUberBufferInterface * GetMeshLightClusterHeaderUberBuffer() const {
        return mesh_light_cluster_header_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetMeshLightClusterNodeUberBuffer() const {
        return mesh_light_cluster_node_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetMeshLightInstanceUberBuffer() const {
        return mesh_light_instance_uber_buffer_.Raw();
    }

    FORCEINLINE RHIBuffer * GetVolumePrimitivesHeaderBuffer() const {
        return volume_primitives_header_buffer_.Raw();
    }

    FORCEINLINE RHIBuffer * GetVolumeGridHeaderBuffer() const {
        return volume_grid_header_buffer_.Raw();
    }
    FORCEINLINE RHIBuffer * GetGaussianRadianceFieldHeaderBuffer() const {
        return gaussian_radiance_field_header_buffer_.Raw();
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
    // Header for geometries.
    // A geometry header holds DeviceGeometryHeader structs.
    TRef<RHIBuffer> geometry_header_buffer_;
    // A static mesh header buffer holding StaticMeshHeader structs.
    TRef<RHIBuffer> static_mesh_header_buffer_;
    // A static mesh description heap (single block buffer heap), use the offsets in static mesh header to access the descriptions.
    TRef<DeviceUberBufferInterface> static_mesh_description_uber_buffer_;
    // A buffer holding the volume primitives headers. (VolumePrimitivesHeader)
    TRef<RHIBuffer> volume_primitives_header_buffer_;
    // A buffer holding the Gaussian Radiance Field headers.
    TRef<RHIBuffer> gaussian_radiance_field_header_buffer_;
    // A buffer holding the volume grid headers. (VolumeGridHeader)
    TRef<RHIBuffer> volume_grid_header_buffer_;

    // A buffer holding all area lights (RawLight structs).
    TRef<DeviceUberBufferInterface> area_lights_uber_buffer_;

    // Mesh light hierarchy buffers.
    TRef<DeviceUberBufferInterface> mesh_light_cluster_header_uber_buffer_;
    TRef<DeviceUberBufferInterface> mesh_light_cluster_node_uber_buffer_;
    TRef<DeviceUberBufferInterface> mesh_light_instance_uber_buffer_;

    // Custom buffer heaps for custom resources (e.g. custom renderable class)
    std::map<uint32_t, TRef<DeviceBufferHeapInterface>> custom_buffer_heaps_;
    // Custom uber buffers for custom resources (e.g. custom renderable class)
    std::map<uint32_t, TRef<DeviceUberBufferInterface>> custom_uber_buffers_;

    // Slot allocators for bindless resources
    SlotAllocator material_slots_, geometry_slots_, static_mesh_slots_, volume_primitives_slots_, volume_grid_slots_, gaussian_radiance_field_slots_;

    // Central delayed destruction ring.
    // Stores resources whose refcount already reached 0 and are safe to delete after N frames.
    DelayedDestructionQueue delayed_destruction_ {2};
};

MI_NAMESPACE_END

#endif //MI_RESOURCE_ALLOCATOR_H
