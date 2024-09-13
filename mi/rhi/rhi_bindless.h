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

#include "rhi/rhi_common.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_desc.h"
#include "core/base.h"

MI_NAMESPACE_BEGIN

class RHIBindlessResourceSlotKeeper : public NonCopyable, public NonMovable {
public:
    ~RHIBindlessResourceSlotKeeper();

    FORCEINLINE RHIBindlessResourceType GetType () const { return type_; }
    FORCEINLINE uint32_t GetSlot () const { return slot_; }

    FORCEINLINE uint32_t IncRef() {
        return ++ref_count_;
    }

    FORCEINLINE uint32_t DecRef() {
        ref_count_--;
        if (ref_count_ == 0) {
            delete this;
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

typedef TRef<RHIBindlessResourceSlotKeeper> RHIBindlessSlotRef;

// A manager allocating indices for each kind of resource every frame
class RHIBindlessManager {
protected:
    RHIBindlessManager() ;
    virtual ~RHIBindlessManager() = default;
public:
    
    RHIBindlessSlotRef AllocateResourceSlot (const RHIBindlessResourceDesc & desc) ;
    void UpdateResourceSlot (RHIBindlessResourceSlotKeeper * slot, const RHIBindlessResourceDesc & desc) ;

protected:
    friend class RHIBindlessResourceSlotKeeper;

    // Thread safe
    void AllocateResourceSlot (const RHIBindlessResourceDesc & desc, RHIBindlessResourceSlotKeeper * out_slot) ;
    // Thread safe
    void FreeResourceSlot (RHIBindlessResourceSlotKeeper * slot) ;

    // Implemented by the RHI backend
    virtual void UpdateResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot) = 0;

    struct BindlessResourceChannel {
        int size;
        // Keep descriptions for each slot
        std::unique_ptr<RHIBindlessResourceDesc[]> desc;
        // References to keep resources alive
        std::unique_ptr<TRef<RHIResource>> resource_refs;
        // Indices unused
        std::unique_ptr<int[]> unused;
        int unused_count;
    } bindless_channels_[(size_t)RHIBindlessResourceType::kMax];

    // Critical section for modifying bindless_channels_
    std::mutex mutex_;
};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_BINDLESS_H
