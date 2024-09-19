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
#include "rhi/rhi.h"
#include "rhi/rhi_resource.h"
#include "rhi/rhi_bindlesskeeper.h"

MI_NAMESPACE_BEGIN

struct RHIBindlessResourceDesc {
    RHIBindlessResourceType type;
    // Allocate a number of consecutive slots for the resource. Default is 1.
    // Used for creating bindless atlas.
    int num_slots {1};
};

inline RHIBindlessResourceDesc RHIBufferBindlessSlotDesc (int num_slots = 1) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kStorageBuffer;
    desc.num_slots = num_slots;
    return desc;
}

inline RHIBindlessResourceDesc RHIUniformBindlessSlotDesc (int num_slots = 1) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kUniformBuffer;
    desc.num_slots = num_slots;
    return desc;
}

inline RHIBindlessResourceDesc RHIUAVBindlessSlotDesc (int num_slots = 1) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kUAV;
    desc.num_slots = num_slots;
    return desc;
}

inline RHIBindlessResourceDesc RHISRVBindlessSlotDesc (int num_slots = 1) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kSRV;
    desc.num_slots = num_slots;
    return desc;
}

inline RHIBindlessResourceDesc RHISamplerBindlessSlotDesc (int num_slots = 1) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kSampler;
    desc.num_slots = num_slots;
    return desc;
}

inline RHIBindlessResourceDesc RHIAccelerationStructureBindlessSlotDesc (int num_slots = 1) {
    RHIBindlessResourceDesc desc;
    desc.type = RHIBindlessResourceType::kAccelerationStructure;
    desc.num_slots = num_slots;
    return desc;
}

// A manager allocating indices for each kind of resource every frame
class RHIBindlessManager {
protected:
    RHIBindlessManager() ;
public:

    virtual ~RHIBindlessManager() = default;

    template<typename T>
    RHIBindlessSlotRef<T> AllocateResourceSlot(const RHIBindlessResourceDesc &desc) {
        auto slot = (RHIBindlessSlotKeeperBase*)GetInfra().Allocate(sizeof(RHIBindlessSlotKeeperBase));
        new(slot) RHIBindlessSlotKeeper<T>();
        AllocateResourceSlot(desc, slot);
        auto ptr = (RHIBindlessSlotKeeper<T>*)slot;
        return RHIBindlessSlotRef<T>(ptr);
    }

    // Resource slot changes should better be batched at frame begin / end.
    // Otherwise, it's not efficient and may cause synchronization bugs.
    // Notify the bindless manager that certain resource slots are updated. The table should be updated.
    void CommitResourceSlotUpdate (RHIBindlessSlotKeeperBase * slot) ;

    // Resource slot changes should better be batched at frame begin / end.
    // Otherwise, it's not efficient and may cause synchronization bugs.
    // Notify the bindless manager that certain resource slots are updated. The table should be updated.
    void CommitResourceSlotUpdate  (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots = 1) ;

    // Called on RHI frame swapping. It's just the time for bindless descriptor set swapping.
    virtual void SwapSets_RHIThread () = 0;
protected:
    friend class RHIBindlessSlotKeeperBase;
    template<typename T>
    friend class RHIBindlessSlotKeeper;

    void AllocateResourceSlot (const RHIBindlessResourceDesc & desc, RHIBindlessSlotKeeperBase * out_slot) ;
    void FreeResourceSlot (RHIBindlessSlotKeeperBase * slot) ;

    // Implemented by the RHI backend
    virtual void CommitResourceSlotUpdateRHI (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots = 1) = 0;
    virtual void FreeResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) = 0;

    struct BindlessResourceChannel {
        // Number of slots
        int size;
        // Keep descriptions for each slot
        int num_slots[C::kMaxNumBindlessResourceSlotsPerChannel];
        // References to keep resources alive
        TRef<RHIResource> resource_refs[C::kMaxNumBindlessResourceSlotsPerChannel];
        // Indices unused
        int unused[C::kMaxNumBindlessResourceSlotsPerChannel];
        int unused_count;
    } bindless_channels_[(size_t)RHIBindlessResourceType::kMaxAndImmSampler];
    // Provide extra info about bindless buffer channels to support RHIBufferSpan
    struct BindlessBufferChannel {
        size_t            offsets[C::kMaxNumBindlessResourceSlotsPerChannel];
        // -1 means the whole buffer
        size_t            sizes[C::kMaxNumBindlessResourceSlotsPerChannel];
    } bindless_buffer_channel[2]; // 0 for storage buffer, 1 for uniform buffer

};
MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_BINDLESS_H
