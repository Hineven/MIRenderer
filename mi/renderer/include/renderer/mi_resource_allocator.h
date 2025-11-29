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

#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>

#include <core/base.h>
#include <core/util/slot_allocator.h>
#include <rhi/rhi_desc.h>
#include <renderer/mi_renderer_fwd.h>
#include <renderer/mi_buffer_heap.h>
#include "../shaders/shared/SharedStaticMesh.hlsl"

MI_NAMESPACE_BEGIN

// Allocate GPU resources used for common bindless rendering (device geometries, materials, static meshes, etc)
// Resources that does not need to be bindless, or already bindless via RHI layers (for example, textures) should
// be allocated separately.
// One allocator for one renderer.
class DeviceBindlessResourceAllocator : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    DeviceBindlessResourceAllocator ();
    ~DeviceBindlessResourceAllocator();

    friend class Renderer;


    static constexpr uint32_t kMaxNumMaterials = 1024;
    static constexpr uint32_t kMaxNumGeometries = 64 * 1024; // 64K geometries
    static constexpr uint32_t kMaxNumStaticMeshes = 64 * 1024;
    // static constexpr uint32_t kMaxNumStaticMeshGeometryMaterialPairs = 256 * 1024;
    static constexpr uint32_t kMaxNumVolumePrimitiveGroups = 1024; // 1K volume primitive groups (assume that there are not many)
    static constexpr uint32_t kMaxNumGaussianRadianceFields = 256; // Assume fewer GRF datasets

    FORCEINLINE DeviceUberBufferInterface * GetVertexUberBuffer () const {
        return vertex_uber_buffer_.Raw();
    }

    FORCEINLINE DeviceUberBufferInterface * GetIndexUberBuffer () const {
        return index_uber_buffer_.Raw();
    }

    FORCEINLINE void RegisterCustomBufferHeap (uint32_t index, DeviceBufferHeapInterface * heap) {
        assert(custom_buffer_heaps_.find(index) == custom_buffer_heaps_.end() && "Custom buffer heap already registered for this index.");
        custom_buffer_heaps_[index] = heap;
    }

    FORCEINLINE DeviceBufferHeapInterface * GetCustomBufferHeap (uint32_t index) const {
        return custom_buffer_heaps_.at(index).Raw();
    }

    FORCEINLINE void RegisterCustomUberBuffer (uint32_t index, DeviceUberBufferInterface * uber_buffer) {
        assert(custom_uber_buffers_.find(index) == custom_uber_buffers_.end() && "Custom uber buffer already registered for this index.");
        custom_uber_buffers_[index] = uber_buffer;
    }

    FORCEINLINE DeviceUberBufferInterface * GetCustomUberBuffer (uint32_t index) const {
        auto it = custom_uber_buffers_.find(index);
        if (it != custom_uber_buffers_.end()) {
            return it->second.Raw();
        }
        return nullptr;
    }

    // Allocate a vertex buffer from the vertex buffer heap.
    FORCEINLINE std::pair<TRef<DeviceUberBufferAllocation>, bool> AllocateVertexBuffer (uint32_t size, bool allow_reallocation = true) {
        return vertex_uber_buffer_->AllocateRefCounted(size, allow_reallocation);
    }
    FORCEINLINE std::pair<TRef<DeviceUberBufferAllocation>, bool> AllocateIndexBuffer (uint32_t size, bool allow_reallocation = true) {
        return index_uber_buffer_->AllocateRefCounted(size, allow_reallocation);
    }

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

    FORCEINLINE RHIBuffer * GetVolumePrimitivesHeaderBuffer() const {
        return volume_primitives_header_buffer_.Raw();
    }

    FORCEINLINE RHIBuffer * GetGaussianRadianceFieldHeaderBuffer() const {
        return gaussian_radiance_field_header_buffer_.Raw();
    }

protected:



    // Underlying buffer holding the material headers. This is updated on a per-frame basis.
    // Allocated a proper size upon construction.
    TRef<RHIBuffer> material_header_buffer_;

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

    // A buffer holding all area lights (RawLight structs).
    TRef<DeviceUberBufferInterface> area_lights_uber_buffer_;

    // Custom buffer heaps for custom resources (e.g. custom renderable class)
    std::map<uint32_t, TRef<DeviceBufferHeapInterface>> custom_buffer_heaps_;
    // Custom uber buffers for custom resources (e.g. custom renderable class)
    std::map<uint32_t, TRef<DeviceUberBufferInterface>> custom_uber_buffers_;

    // Slot allocators for bindless resources
    SlotAllocator material_slots_, geometry_slots_, static_mesh_slots_, volume_primitives_slots_, gaussian_radiance_field_slots_;

};

MI_NAMESPACE_END

#endif //MI_RESOURCE_ALLOCATOR_H
