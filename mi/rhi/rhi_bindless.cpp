/*
 * Created: 2024/7/10
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <span>
#include "rhi_bindless.h"
#include "core/infra.h"
#include "core/constants.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

RHIBindlessSlotKeeper::~RHIBindlessSlotKeeper() {
    RHI::Get().GetBindlessManager().FreeResourceSlot(this);
}

RHIBindlessSlotRef RHIBindlessManager::AllocateResourceSlot(const RHIBindlessResourceDesc &desc) {
    if(desc.type == RHIBindlessResourceType::kImmutableSampler) {
        mi_assert(false, "Currently immutable samplers are pre defined and can't be modified.");
    }
    auto slot = GetInfra().New<RHIBindlessSlotKeeper>();
    AllocateResourceSlot(desc, slot);
    UpdateResourceSlotRHI(desc.type, slot->slot_);
    return {slot};
}

void RHIBindlessManager::UpdateResourceSlot(RHIBindlessSlotKeeper *slot,
                                            const RHIBindlessResourceDesc &desc) {
    if(desc.type == RHIBindlessResourceType::kImmutableSampler) {
        mi_assert(false, "Currently immutable samplers are pre defined and can't be modified.");
    }
    std::lock_guard lock(mutex_);
    auto & channel = bindless_channels_[(uint32_t)desc.type];
    auto & old_desc = channel.desc[slot->slot_];
    mi_assert(old_desc.type == desc.type, "Resource type mismatch");
    UpdateResourceSlotRHI(desc.type, slot->slot_);
    old_desc = desc;
}

RHIBindlessManager::RHIBindlessManager() {
    auto support = RHI::Get().QueryRHIBindlessSupportInfo();
    auto SetupBindlessChannel = [&](RHIBindlessResourceType channel_type) {
        auto & channel = bindless_channels_[(uint32_t)channel_type];
        int limit = 0;
        if(channel_type == RHIBindlessResourceType::kSampler)
            limit = support.max_num_sampler_slots;
        else if(channel_type == RHIBindlessResourceType::kImmutableSampler)
            limit = std::min(support.max_num_immutable_sampler_slots, C::kNumDefaultBindlessImmutableSamplers);
        else if(channel_type == RHIBindlessResourceType::kAccelerationStructure)
            limit = std::min(support.max_num_resource_slots, C::kMaxNumBindlessAccelerationStructures);
        else limit = support.max_num_resource_slots;
        limit = std::min(limit, (int)C::kMaxNumBindlessResourceSlotsPerChannel);
        channel.size   = limit;
        channel.unused_count = limit;
        for(int i = 0; i < channel.unused_count; i++)
            channel.unused[i] = channel.unused_count - i - 1;
    };
    for(int i = 0; i < (int)RHIBindlessResourceType::kMax; i++) {
        SetupBindlessChannel((RHIBindlessResourceType)i);
    }
}

void RHIBindlessManager::AllocateResourceSlot(const RHIBindlessResourceDesc & desc, RHIBindlessSlotKeeper * out_slot) {
    std::lock_guard lock(mutex_);
    auto & channel = bindless_channels_[(uint32_t)desc.type];
    mi_assert(channel.unused_count >= 1, "Not enough bindless slots");
    out_slot->slot_ = channel.unused[--channel.unused_count];
    out_slot->type_ =  desc.type;
}

void RHIBindlessManager::FreeResourceSlot(RHIBindlessSlotKeeper * slot) {
    std::lock_guard lock(mutex_);
    auto & channel = bindless_channels_[(uint32_t)slot->type_];
    channel.unused[channel.unused_count++] = slot->slot_;
    FreeResourceSlotRHI(slot->type_, slot->slot_);
}


MI_NAMESPACE_END



