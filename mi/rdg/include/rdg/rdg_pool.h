/*
 * Created: 2025/3/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_POOL_H
#define RDG_POOL_H

#include <map>
#include "core/util/alloc.h"
#include "rdg/rdg_base.h"

MI_NAMESPACE_BEGIN

class RHITexture;
class RHIBuffer;


// RDG resources allocated from the pool will keep a reference to it, preventing it from being released.
class RDGResourcePool : public RefCounted<false>, public NonMovable, public NonCopyable {
protected:
    TOneTimeLinearAllocator<> buffer_allocator_;
    RDGResourcePool ();
    ~RDGResourcePool ();
public:
    template<CReferenceCounted T>
    friend class TRef;
    static TRef<RDGResourcePool> Create () ;

    // 128MB
    constexpr static uint32_t kBufferBlockSizeLog2 = 27;
    constexpr static size_t kBufferBlockSize = 1ull << kBufferBlockSizeLog2;
    constexpr static uint32_t kBufferReusingThresholdLog2 = 4;
    // Do not reuse buffers larger than 16MB (which increases overall memory consumption)
    constexpr static uint32_t kBufferReusingAbsoluteThresholdLog2 = 24;

    FORCEINLINE TOneTimeLinearAllocator<> & GetBufferAllocator() {
        return buffer_allocator_;
    }
    // Generic allocation function.
    FORCEINLINE void * Allocate (size_t size) {
        return GetBufferAllocator().Allocate(size);
    }
    // Allocate a piece of frame local host buffer memory. Very fast linear allocation. Use this
    // function to allocate frame temporaries. The memory will be automatically freed when the command buffer is reset.
    // (That is, when the frame ends.)
    template<CMemTrivial T>
    FORCEINLINE T * Allocate (auto...args) {
        auto ptr = GetBufferAllocator().Allocate(sizeof(T));
        return new(ptr) T(args...);
    }
    // Allocate a piece of frame local host buffer memory. Very fast linear allocation. Use this
    // function to allocate frame temporaries. The memory will be automatically freed when the command buffer is reset.
    // (That is, when the frame ends.)
    template<CAOUB T>
    FORCEINLINE std::remove_all_extents_t<T> * Allocate (size_t count) {
        using TElem = std::remove_all_extents_t<T>;
        auto ptr = GetBufferAllocator().Allocate(sizeof(TElem) * count);
        return new(ptr) TElem[count];
    }

    // Called by RDG before pass execution. Map real RHI resources to the RDG resources.
    void AllocateResource (RDGTexture * texture) ;
    // Note: uniform and staging buffers should never be transient across frames. The correct behavior
    // is to allocate them on each frame (if you have demands).
    void AllocateResource (RDGBuffer * buffer) ;

    // Allocate a uniform buffer (and will never be recycled within the frame)
    void AllocateUniformBuffer (RDGBuffer * buffer) ;
    // Allocate a staging buffer (and will never be recycled within the frame)
    void AllocateStagingBuffer (RDGBuffer * buffer) ;

    // Move to next frame, and free the resources for the previous frame.
    void NewFrame ();

    // Recycle the RDG resources that are no longer used (reference count approaching 0), unlink RHI resources for further reuse.
    void RecycleResource (RDGTexture * texture) ;
    void RecycleResource (RDGBuffer * buffer) ;

protected:

    // Map descriptor hash to underlying texture index
    std::map<uint32_t, std::vector<RHITexture*> > rhi_free_texture_map_;
    std::vector<RHITexture*> rhi_allocated_textures_;
    // Keep references.
    std::vector<TRef<RHITexture>> rhi_texture_references_;

    // Some of the buffers have simpler allocation and recycling rules
    // for performance or synchronization considerations. They may not be transient across frames.
    // They are not recycled within a frame, and will be recycled after the frame completed
    // running on GPU. It is designed assuming that no two frames will be concurrently running on the GPU.
    struct OneTimeUseBufferPool {
        std::vector<TRef<RHIBuffer>> rhi_pool_buffer_references;
        // Current top of the uniform buffer
        uint32_t buffer_index {};
        uint32_t buffer_offset {};
        // Upon frame end, all the allocated buffers in the vector should have only 1 reference.
        // The pool will check for that.
        std::vector<TRef<RDGBuffer>> allocated_buffer_references;
    } uniforms[2], staging[2];

    int one_time_use_buffer_pool_index_ {};

    void AllocateOneTimeUseBuffer (OneTimeUseBufferPool & pool, RDGBuffer * buffer) ;

    // Map descriptor hash to underlying buffer index
    std::map<uint32_t, std::vector<RHIBuffer*> > rhi_free_buffer_map_;
    std::vector<RHIBuffer*> rhi_allocated_buffers_;
    // Keep references.
    std::vector<TRef<RHIBuffer>> rhi_buffer_references_;

};

MI_NAMESPACE_END
#endif //RDG_POOL_H
