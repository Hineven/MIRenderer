/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_BINDLESS_H
#define MIRENDERERDEV_RHI_BINDLESS_H

#include <vector>
#include <mutex>
#include "core/pixel_format.h"
#include "core/constants.h"

#include "rhi/rhi_common.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_desc.h"
#include "core/base.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

class RHIBindlessSlotKeeper : public NonCopyable, public NonMovable {
public:
    ~RHIBindlessSlotKeeper();

    FORCEINLINE RHIBindlessResourceType GetType () const { return type_; }
    FORCEINLINE uint32_t GetSlot () const { return slot_; }

    FORCEINLINE uint32_t IncRef() {
        return ++ref_count_;
    }

    FORCEINLINE uint32_t DecRef() {
        ref_count_--;
        if (ref_count_ == 0) {
            // Self destruct
            GetInfra().Delete(this);
        }
        return ref_count_;
    }

    FORCEINLINE uint32_t GetRefCount() const {
        return ref_count_;
    }
protected:
    friend class RHIBindlessManager;

    RHIBindlessResourceType type_;
    uint32_t slot_;

    int ref_count_;
};

struct RHIBindlessResourceDesc {
    RHIBindlessResourceType type;
    union {
        RHIBufferSpan buffer;
        RHITexture  * texture;
        RHISampler  * sampler;
        RHIAccelerationStructure * accel;
    } detail;
};

inline RHIBindlessResourceDesc RHIBufferBindlessSlotDesc (RHIBufferSpan span) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kStorageBuffer;
    desc.detail.buffer = span;
    return desc;
}

inline RHIBindlessResourceDesc RHIUniformBindlessSlotDesc (RHIBufferSpan span) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kUniformBuffer;
    desc.detail.buffer = span;
    return desc;
}

template<PixelFormatType Type>
inline RHIBindlessResourceDesc RHIUAVBindlessSlotDesc (RHITexture * texture) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kUAV;
    desc.detail.texture = texture;
    return desc;
}

inline RHIBindlessResourceDesc RHISRVBindlessSlotDesc (RHITexture * texture) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kSRV;
    desc.detail.texture = texture;
    return desc;
}

inline RHIBindlessResourceDesc RHISamplerBindlessSlotDesc (RHISampler * sampler) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kSampler;
    desc.detail.sampler = sampler;
    return desc;
}

inline RHIBindlessResourceDesc RHIAccelerationStructureBindlessSlotDesc (RHIAccelerationStructure * accel) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kAccelerationStructure;
    desc.detail.accel = accel;
    return desc;
}

typedef TRef<RHIBindlessSlotKeeper> RHIBindlessSlotRef;

// A manager allocating indices for each kind of resource every frame
class RHIBindlessManager {
protected:
    RHIBindlessManager() ;
public:

    virtual ~RHIBindlessManager() = default;
    RHIBindlessSlotRef AllocateResourceSlot (const RHIBindlessResourceDesc & desc) ;
    void UpdateResourceSlot (RHIBindlessSlotKeeper * slot, const RHIBindlessResourceDesc & desc) ;

    // Called on RHI frame swapping. It's just the time for bindless descriptor set swapping.
    virtual void SwapSets_RHIThread () = 0;
protected:
    friend class RHIBindlessSlotKeeper;

    // Thread safe
    void AllocateResourceSlot (const RHIBindlessResourceDesc & desc, RHIBindlessSlotKeeper * out_slot) ;
    // Thread safe
    void FreeResourceSlot (RHIBindlessSlotKeeper * slot) ;

    // Implemented by the RHI backend
    virtual void UpdateResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot) = 0;
    virtual void FreeResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot) = 0;

    struct BindlessResourceChannel {
        // Number of slots
        int size;
        // Keep descriptions for each slot
        RHIBindlessResourceDesc desc[C::kMaxNumBindlessResourceSlotsPerChannel];
        // References to keep resources alive
        TRef<RHIResource> resource_refs[C::kMaxNumBindlessResourceSlotsPerChannel];
        // Indices unused
        int unused[C::kMaxNumBindlessResourceSlotsPerChannel];
        int unused_count;
    } bindless_channels_[(size_t)RHIBindlessResourceType::kMax];


    // Critical section for modifying bindless_channels_
    std::mutex mutex_;
};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_BINDLESS_H
