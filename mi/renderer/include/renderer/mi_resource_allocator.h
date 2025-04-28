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

MI_NAMESPACE_BEGIN

// Interface for resource level buffer allocator
class GPUBufferHeapInterface : public NonMovable, public RefCounted<> {
public:
    friend class GPUBufferHeapBuffer;
    GPUBufferHeapInterface (RHIBufferUsageFlags usage, uint32_t alignment) : usage_(usage), allocation_alignment(alignment) {}
    virtual RHIBufferSpan Allocate (uint32_t size) = 0;
    // Allocate a reference counted buffer.
    TRef<GPUBufferHeapBuffer> AllocateRefCounted (uint32_t size) ;
    virtual void Free (RHIBufferSpan allocation) ;
    FORCEINLINE uint32_t GetAllocationAlignment () const {
        return allocation_alignment;
    }
    virtual ~GPUBufferHeapInterface () = default;
protected:

    RHIBufferUsageFlags usage_;
    uint32_t allocation_alignment {};
};

class GPUBufferHeapBuffer : public NonMovable, public RefCounted<> {
protected:
    FORCEINLINE GPUBufferHeapBuffer () = default;
    RHIBufferSpan buffer {};
    GPUBufferHeapInterface * heap {};
    friend class GPUBufferHeapInterface;
public:
    ~GPUBufferHeapBuffer();
    FORCEINLINE GPUBufferHeapInterface * GetHeap () const {return heap; }
    FORCEINLINE RHIBufferSpan GetRHI () const {return buffer; }
};

// A very simple buffer heap for grouping up one kind of memory in buffer resource level
class SimpleGPUBufferHeap : public GPUBufferHeapInterface {
protected:
    uint32_t buffer_block_size_ {};
    struct BufferBlock {
        TRef<RHIBuffer> buffer;
        struct Segment {
            uint32_t start_offset {};
            // This does not affect orders so mutable.
            mutable uint32_t size {};
            FORCEINLINE bool operator < (const Segment & rhs) const {
                return start_offset < rhs.start_offset;
            }
        };
        std::set<Segment> free_segments_;
    };
    std::vector<BufferBlock> buffer_blocks_;
    int FindBufferBlockIndex (RHIBuffer * buffer) const ;
public:
    SimpleGPUBufferHeap (RHIBufferUsageFlags usage, uint32_t allocation_alignment, uint32_t buffer_block_size = 256 * 1024 * 1024);
    ~SimpleGPUBufferHeap();
    RHIBufferSpan Allocate (uint32_t size) override;
    void Free (RHIBufferSpan allocation) override;
protected:
    std::mutex mutex_;
};

// Allocate grouped GPU resources used for rendering (geometry buffers, materials, etc)
// Resources that does not need to be grouped (textures, etc) should be allocated separately.
class GroupedRenderResourceAllocator : public NonCopyable, public NonMovable {
public:
    GroupedRenderResourceAllocator (
        GPUBufferHeapInterface * vertex_buffer_heap,
        GPUBufferHeapInterface * index_buffer_heap
    );
    friend class Geometry;
    friend class DeviceGeometry;
    friend class DeviceMaterial;
    friend class BindlessDeviceTexture;

    constexpr uint32_t kMaxNumMaterials = 1024;

protected:

    // All device materials allocated
    std::vector<TRef<DeviceMaterial>> materials_;
    // Underlying buffer holding the material headers. This is updated on a per-frame basis.
    // Allocated a proper size upon construction.
    TRef<RHIBuffer> material_buffer_;
    // Slots (indices) for unused materials. Initialized to kMaxNumMaterials elements upon construction.
    std::stack<uint32_t> free_material_slots_;

    // Heaps for consistent geometry
    TRef<GPUBufferHeapInterface> vertex_buffer_heap_;
    TRef<GPUBufferHeapInterface> index_buffer_heap_;

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
