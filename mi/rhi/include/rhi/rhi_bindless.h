/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_BINDLESS_H
#define MIRENDERERDEV_RHI_BINDLESS_H

#include <vector>
#include <mutex>
#include <set>

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
};

FORCEINLINE RHIBindlessResourceDesc RHIBufferBindlessSlotDesc () {
    RHIBindlessResourceDesc desc {};
    desc.type = RHIBindlessResourceType::kReadOnlyStorageBuffer;
    return desc;
}
FORCEINLINE RHIBindlessResourceDesc RHISRVBindlessSlotDesc () {
    RHIBindlessResourceDesc desc {};
    desc.type = RHIBindlessResourceType::kSRV;
    return desc;
}

FORCEINLINE RHIBindlessResourceDesc RHIAccelerationStructureBindlessSlotDesc () {
    RHIBindlessResourceDesc desc {};
    desc.type = RHIBindlessResourceType::kAccelerationStructure;
    return desc;
}

struct RHIPackedBindlessSlot {
    RHIBindlessResourceType type : 4;
    uint32_t slot_index : 28;
    FORCEINLINE static RHIPackedBindlessSlot Pack (RHIBindlessResourceType t, uint32_t slot_idx) {
        RHIPackedBindlessSlot slot = {};
        slot.type = t;
        slot.slot_index = slot_idx;
        return slot;
    }
    FORCEINLINE bool operator < (const RHIPackedBindlessSlot & rhs) const {
        return *reinterpret_cast<const uint32_t*>(this) < *reinterpret_cast<const uint32_t*>(&rhs);
    }
};

template<typename T>
struct TGetBindlessResourceType {
    static constexpr RHIBindlessResourceType value = RHIBindlessResourceType::kMax;
};
template<>
struct TGetBindlessResourceType<RHIBuffer> {
    static constexpr RHIBindlessResourceType value = RHIBindlessResourceType::kReadOnlyStorageBuffer;
};
template<>
struct TGetBindlessResourceType<RHITexture> {
    static constexpr RHIBindlessResourceType value = RHIBindlessResourceType::kSRV;
};
template<>
struct TGetBindlessResourceType<RHIAccelerationStructure> {
    static constexpr RHIBindlessResourceType value = RHIBindlessResourceType::kAccelerationStructure;
};

// A manager allocating indices for each kind of resource every frame
class RHIBindlessManager {
protected:
    RHIBindlessManager() ;
public:
    friend class RHI;

    virtual ~RHIBindlessManager() = default;

    template<typename T>
    RHIBindlessSlotRef<T> AllocateResourceSlot() {
        auto slot = (RHIBindlessSlotKeeperBase*)new RHIBindlessSlotKeeper<T>();
        constexpr auto type = TGetBindlessResourceType<T>::value;
        static_assert(type != RHIBindlessResourceType::kMax, "Invalid bindless resource type");
        AllocateResourceSlot(RHIBindlessResourceDesc{type}, slot);
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

    // This is not performant. For convenience only.
    void CommitResourceSlotUpdate (RHIBindlessResourceType type, std::span<const uint32_t> slot_indices) ;

    // Called on RHI frame swapping. It's just the time for bindless descriptor set swapping.
    virtual void SwapSets_RHIThread (std::span<RHIPackedBindlessSlot> slots_to_free) = 0;

protected:
    friend class RHIBindlessSlotKeeperBase;
    template<typename T>
    friend class RHIBindlessSlotKeeper;

    void AllocateResourceSlot (const RHIBindlessResourceDesc & desc, RHIBindlessSlotKeeperBase * out_slot) ;
    void FreeResourceSlotFromTable (RHIBindlessSlotKeeperBase * slot);
    void FreeResourceSlot (RHIBindlessSlotKeeperBase * slot) ;
    void FreeResourceSlot_Delayed (RHIBindlessSlotKeeperBase * slot) ;

    // Implemented by the RHI backend
    virtual void CommitResourceSlotUpdateRHI (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) = 0;
    virtual void FreeResourceSlotRHI (RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) = 0;

    struct BindlessResourceChannel {
        // References to keep resources alive
        TRef<RHIResource> resource_refs[C::kMaxNumBindlessResourceSlotsPerChannel];
        // Indices unused
        int unused[C::kMaxNumBindlessResourceSlotsPerChannel];
        uint32_t unused_count;
        uint32_t total_count;
    } bindless_channels_[(size_t)RHIBindlessResourceType::kMax];

    // Keep the indices of the freed slots delayed for RHI cleaning.
    std::set<RHIPackedBindlessSlot> delayed_free_slots_;
    // Execute on render thread.
    std::span<RHIPackedBindlessSlot> PrepareDelayedSlotsForRHIFree();

    // Called before the destruction of RHI. Release all handles we hold.
    void PreDestruction ();
};
MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_BINDLESS_H
