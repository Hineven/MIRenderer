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
#include "rhi/rhi_desc.h"

MI_NAMESPACE_BEGIN

class RHITexture;
class RHIBuffer;
class RDGResourcePool;

// Physical buffer slot. Two states:
//   Active: ref_count > 0, referenced by one or more RDGResources.
//   Free:   ref_count == 0, stored in the pool's free list awaiting reuse.
// last_* tracks the physical resource's access state — used for:
//   - Barrier fallback when RDGResource's own tracking is empty (first use).
//   - Cross-frame propagation (initialized from previous frame's final state).
//   - Intra-frame aliasing (updated by predecessor's release, read by successor's first use).
struct RDGPoolBufferAllocation : public NonCopyable, public NonMovable {
    RHIBuffer * buffer {};
    size_t allocation_size {};
    uint32_t resource_class_hash {};
    uint32_t ref_count {};
    RHIPipelineStageFlags last_read_stages {};
    RHIPipelineStageFlags last_write_stages {};
    RHIGPUAccessFlags last_access {};

    void Acquire () { ref_count++; }
    bool Release () { return --ref_count == 0; }
};

// Physical texture slot. Same two-state model as RDGPoolBufferAllocation.
struct RDGPoolTextureAllocation : public NonCopyable, public NonMovable {
    RHITexture * texture {};
    uint32_t resource_class_hash {};
    uint32_t ref_count {};
    RHIPipelineStageFlags last_read_stages {};
    RHIPipelineStageFlags last_write_stages {};
    RHIGPUAccessFlags last_access {};

    void Acquire () { ref_count++; }
    bool Release () { return --ref_count == 0; }
};

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
    constexpr static size_t kBufferReusingAbsoluteThreshold = 1ull << kBufferReusingAbsoluteThresholdLog2;

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

    // Recycle the RDG resources that are no longer used (reference count approaching 0), unlink RHI resources for further reuse.
    void RecycleResource (RDGTexture * texture) ;
    void RecycleResource (RDGBuffer * buffer) ;

    // --- Resource aliasing (two-phase allocation) ---

    // Phase 1: Record a resource allocation request (dry run, no RHI allocation yet).
    // first_pass / last_pass define the resource's lifetime within the frame.
    void RequestAllocation (RDGBuffer * buffer, uint32_t first_pass, uint32_t last_pass);
    void RequestAllocation (RDGTexture * texture, uint32_t first_pass, uint32_t last_pass);

    // Phase 2: Commit all pending allocations. Performs aliasing analysis on transient
    // resources (non-overlapping lifetimes share physical RHI memory), then allocates
    // physical RHI resources and writes them back to each RDGResource.
    // Export resources are allocated with dedicated slots (no aliasing).
    void CommitAllocations ();

    FORCEINLINE size_t GetTotalDeviceMemoryUsage () const {
        return total_device_memory_usage_;
    }

protected:

    // Allocate a new physical RHI buffer and wrap it in an allocation (ref_count=0, hash unset).
    RDGPoolBufferAllocation * AllocateNewBufferBlock (RHIBufferDesc desc);

    // Search free pool for a matching buffer allocation, falling back to new allocation.
    // Sets resource_class_hash on the returned allocation. Mutates buffer->desc_.size.
    RDGPoolBufferAllocation * FindOrCreateBufferAllocation (RDGBuffer * buffer);

    // Allocate a new physical RHI texture and wrap it in an allocation (ref_count=0, hash unset).
    RDGPoolTextureAllocation * AllocateNewTextureBlock (RHITextureDesc desc, uint32_t hash);

    // Attach an allocation to a RDGResource (Acquire + wire). Resource access tracking starts empty.
    void AttachAllocation (RDGBuffer * buffer, RDGPoolBufferAllocation * alloc);
    void AttachAllocation (RDGTexture * texture, RDGPoolTextureAllocation * alloc);

    // Map descriptor hash to underlying buffer allocations (currently free)
    std::map<uint32_t, std::vector<RDGPoolBufferAllocation*>> free_buffer_allocations_;

    // Keep references of all created RHI buffers. Dont let them be released.
    std::vector<TRef<RHIBuffer>> rhi_buffer_references_;

    // Map descriptor hash to underlying texture allocations (currently free)
    std::map<uint32_t, std::vector<RDGPoolTextureAllocation*>> free_texture_allocations_;
    std::vector<TRef<RHITexture>> rhi_texture_references_;

    // --- Aliasing internal state ---
    struct PendingBufferAllocation {
        RDGBuffer * buffer;
        uint32_t first_pass;
        uint32_t last_pass;
    };
    struct PendingTextureAllocation {
        RDGTexture * texture;
        uint32_t first_pass;
        uint32_t last_pass;
    };
    std::vector<PendingBufferAllocation> pending_buffer_allocations_;
    std::vector<PendingTextureAllocation> pending_texture_allocations_;

    size_t total_device_memory_usage_ {0};

    // Keep track of the number of active (allocated) buffers and textures in the pool.
    uint32_t num_active_buffers_ {0};
    uint32_t num_active_textures_ {0};
};

MI_NAMESPACE_END
#endif //RDG_POOL_H
