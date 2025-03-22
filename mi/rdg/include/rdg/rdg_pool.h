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
    void AllocateResource (RDGBuffer * buffer) ;

    // Allocate a uniform buffer (and will never be recycled)
    void AllocateUniformBuffer (RDGBuffer * buffer) ;

    // Transfer all staging buffers to uniform buffers
    void StageUniformBuffers (RHICommandQueueGraphics & queue) ;

    // Recycle the RDG resources that are no longer used (reference count approaching 0), unlink RHI resources for further reuse.
    void RecycleResource (RDGTexture * texture) ;
    void RecycleResource (RDGBuffer * buffer) ;

protected:

    // Map descriptor hash to underlying texture index
    std::map<uint32_t, std::vector<RHITexture*> > rhi_free_texture_map_;
    std::vector<RHITexture*> rhi_allocated_textures_;
    // Keep references.
    std::vector<TRef<RHITexture>> rhi_texture_references_;

    // The uniform buffer pool
    std::vector<TRef<RHIBuffer>> rhi_uniform_buffer_references_;
    // For each uniform buffer, duplicate a staging buffer for CPU write
    std::vector<TRef<RHIBuffer>> rhi_staging_buffer_references_;
    // Current top of the uniform buffer
    uint32_t rhi_uniform_buffer_index_ {};
    uint32_t rhi_uniform_buffer_offset_ {};
    bool buffers_staged_ {};

    // Map descriptor hash to underlying buffer index
    std::map<uint32_t, std::vector<RHIBuffer*> > rhi_free_buffer_map_;
    std::vector<RHIBuffer*> rhi_allocated_buffers_;
    // Keep references.
    std::vector<TRef<RHIBuffer>> rhi_buffer_references_;

};

MI_NAMESPACE_END
#endif //RDG_POOL_H
