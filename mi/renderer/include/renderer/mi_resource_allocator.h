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

#include "core/base.h"
#include "renderer/mi_renderer_fwd.h"
#include "rhi/rhi_bindlesskeeper.h"
#include "rhi/rhi_desc.h"
#include "renderer/mi_buffer_heap.h"

MI_NAMESPACE_BEGIN

// Allocate grouped GPU resources used for common rendering (device geometries, materials, etc)
// Resources that does not need to be grouped or indexed (textures, etc) should be allocated separately.
// One allocator per renderer, at the highest level hierarchy.
class CommonGroupedDeviceResourceAllocator : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    CommonGroupedDeviceResourceAllocator (
        DeviceBufferHeapInterface * vertex_buffer_heap,
        DeviceBufferHeapInterface * index_buffer_heap
    );
    ~CommonGroupedDeviceResourceAllocator();

    friend class Geometry;
    friend class DeviceGeometry;
    friend class Material;
    friend class DeviceMaterial;
    friend class BindlessDeviceTexture;
    friend class Renderer;

    static constexpr uint32_t kMaxNumMaterials = 1024;

    FORCEINLINE DeviceBufferHeapInterface * GetVertexBufferHeap () const {
        return vertex_buffer_heap_.Raw();
    }

    FORCEINLINE DeviceBufferHeapInterface * GetIndexBufferHeap () const {
        return index_buffer_heap_.Raw();
    }

    FORCEINLINE void RegisterCustomBufferHeap (uint32_t index, DeviceBufferHeapInterface * heap) {
        assert(custom_buffer_heaps_.find(index) == custom_buffer_heaps_.end() && "Custom buffer heap already registered for this index.");
        custom_buffer_heaps_[index] = heap;
    }

    FORCEINLINE DeviceBufferHeapInterface * GetCustomBufferHeap (uint32_t index) const {
        return custom_buffer_heaps_.at(index).Raw();
    }

protected:

    // All device materials allocated
    std::vector<TRef<DeviceMaterial>> materials_;
    // Underlying buffer holding the material headers. This is updated on a per-frame basis.
    // Allocated a proper size upon construction.
    TRef<RHIBuffer> material_header_buffer_;
    // Slots (indices) for unused materials. Initialized to kMaxNumMaterials elements upon construction.
    std::stack<uint32_t> free_material_slots_;

    // Heaps for consistent geometries
    TRef<DeviceBufferHeapInterface> vertex_buffer_heap_;
    TRef<DeviceBufferHeapInterface> index_buffer_heap_;

    // Custom buffer heaps for custom resources (e.g. custom renderable class)
    std::map<uint32_t, TRef<DeviceBufferHeapInterface>> custom_buffer_heaps_;

    FORCEINLINE RHIBufferSpan AllocateVertexBuffer (uint32_t size) {
        return vertex_buffer_heap_->Allocate(size);
    }
    FORCEINLINE RHIBufferSpan AllocateIndexBuffer (uint32_t size) {
        return index_buffer_heap_->Allocate(size);
    }

    FORCEINLINE void FreeVertexBuffer (RHIBufferSpan buffer) {
        vertex_buffer_heap_->Free(buffer);
    }
    FORCEINLINE void FreeIndexBuffer (RHIBufferSpan buffer) {
        index_buffer_heap_->Free(buffer);
    }

    FORCEINLINE uint32_t AllocateMaterialSlot () {
        if (!free_material_slots_.empty ()) {
            auto idx = free_material_slots_.top();
            free_material_slots_.pop();
            return idx;
        }
        return UINT32_MAX;
    }
    FORCEINLINE void FreeMaterialSlot (uint32_t idx) {
        assert(idx < kMaxNumMaterials);
        free_material_slots_.push(idx);
    }
};

MI_NAMESPACE_END

#endif //MI_RESOURCE_ALLOCATOR_H
