/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RESOURCE_ALLOCATOR_H
#define MI_RESOURCE_ALLOCATOR_H

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

// Allocate grouped GPU resources used for rendering (geometry buffers, materials, etc)
// Resources that does not need to be grouped (textures, etc) should be allocated separately.
class GroupedRenderResourceAllocator : public NonCopyable, public NonMovable {
public:
    GroupedRenderResourceAllocator (
        DeviceBufferHeapInterface * vertex_buffer_heap,
        DeviceBufferHeapInterface * index_buffer_heap
    );
    friend class Geometry;
    friend class DeviceGeometry;
    friend class DeviceMaterial;
    friend class BindlessDeviceTexture;

    static constexpr uint32_t kMaxNumMaterials = 1024;

protected:

    // All device materials allocated
    std::vector<TRef<DeviceMaterial>> materials_;
    // Underlying buffer holding the material headers. This is updated on a per-frame basis.
    // Allocated a proper size upon construction.
    TRef<RHIBuffer> material_header_buffer_;
    // Slots (indices) for unused materials. Initialized to kMaxNumMaterials elements upon construction.
    std::stack<uint32_t> free_material_slots_;

    // Heaps for consistent geometry
    TRef<DeviceBufferHeapInterface> vertex_buffer_heap_;
    TRef<DeviceBufferHeapInterface> index_buffer_heap_;

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
};

MI_NAMESPACE_END

#endif //MI_RESOURCE_ALLOCATOR_H
