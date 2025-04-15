/*
 * Created: 2025/4/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_WORLD_H
#define MI_WORLD_H

#include <set>
#include <vector>
#include <rhi/rhi_texture.h>

#include "core/base.h"
#include "core/refcounted.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

class GPUBufferHeapBuffer;
// Interface for resource level buffer allocator
class GPUBufferHeapInterface : public NonMovable, public RefCounted<> {
public:
    friend class GPUBufferHeapBuffer;
    GPUBufferHeapInterface (RHIBufferUsageFlags usage, uint32_t alignment) : usage_(usage), allocation_alignment(alignment) {}
    virtual TRef<GPUBufferHeapBuffer> Allocate (uint32_t size) = 0;
    FORCEINLINE uint32_t GetAllocationAlignment () const {
        return allocation_alignment;
    }
    virtual ~GPUBufferHeapInterface () = default;
protected:
    virtual void Free (GPUBufferHeapBuffer * buffer) ;

    TRef<GPUBufferHeapBuffer> CreateBuffer (RHIBufferSpan);

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
    TRef<GPUBufferHeapBuffer> Allocate (uint32_t size) override;
protected:
    void Free (GPUBufferHeapBuffer * buffer) override;
    std::mutex mutex_;
};

// Keeping all GPU resources used for rendering (geometries, meshes, materials, etc)
class RenderResourceLibrary : public NonCopyable, public NonMovable {
public:
    RenderResourceLibrary (GPUBufferHeapInterface * vertex_buffer_heap, GPUBufferHeapInterface * index_buffer_heap);

    TRef<Geometry> CreateGeometry () ;
    TRef<Material> CreateMaterial () ;

protected:
    // Bindless texture array for all materials
    std::vector<TRef<RHITexture>> textures_;
    std::vector<TRef<Geometry>> geometries_;
    std::vector<TRef<Material>> materials_;

    // Heaps for consistent geometry
    TRef<GPUBufferHeapInterface> vertex_buffer_heap_;
    TRef<GPUBufferHeapInterface> index_buffer_heap_;
};

// Integrated class managing the world.
class World : public NonCopyable, public NonMovable {
public:

    // Create a static mesh renderable and add it to the world.
    // Releasing the reference yourself will remove it from the renderer.
    TRef<StaticMesh> CreateStaticMeshRenderable () ;
protected:

};

MI_NAMESPACE_END

#endif //MI_WORLD_H
