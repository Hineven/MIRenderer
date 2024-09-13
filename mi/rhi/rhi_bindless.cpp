/*
 * Created: 2024/7/10
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <span>
#include "rhi_bindless.h"
#include "core/infra.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

RHIBindlessResourceSlotKeeper::~RHIBindlessResourceSlotKeeper() {
    RHI::GetInstance().GetBindlessManager().FreeResourceSlot(this);
}

RHIBindlessSlotRef RHIBindlessManager::AllocateResourceSlot(const mi::RHIBindlessResourceDesc &desc) {
    auto slot = new RHIBindlessResourceSlotKeeper();
    AllocateResourceSlot(desc, slot);
    UpdateResourceSlotRHI(desc.type, slot->slot_);
    return {slot};
}

void RHIBindlessManager::UpdateResourceSlot(RHIBindlessResourceSlotKeeper *slot,
                                            const RHIBindlessResourceDesc &desc) {
    std::lock_guard lock(mutex_);
    auto & channel = bindless_channels_[(uint32_t)desc.type];
    auto & old_desc = channel.desc[slot->slot_];
    mi_assert(old_desc.type == desc.type, "Resource type mismatch");
    UpdateResourceSlotRHI(desc.type, slot->slot_);
    old_desc = desc;
}

RHIBindlessManager::RHIBindlessManager() {
    auto support = RHI::GetInstance().QueryRHIBindlessSupportInfo();
    auto SetupBindlessChannel = [&](RHIBindlessResourceType channel_type) {
        auto & channel = bindless_channels_[(uint32_t)channel_type];
        int limit = 0;
        if(channel_type == RHIBindlessResourceType::kSampler)
            limit = support.max_sampler_slots;
        else if(channel_type == RHIBindlessResourceType::kImmutableSampler)
            limit = support.max_immutable_sampler_slots;
        else limit = support.max_resource_slots;
        limit = std::min(limit, (int)CRHIMaxBindlessSlotsPerResourceType);
        channel.desc = std::make_unique<RHIBindlessResourceDesc[]>(limit);
        channel.unused = std::make_unique<int[]>(limit);
        channel.unused_count = limit;
        for(int i = 0; i < channel.unused_count; i++)
            channel.unused[i] = channel.unused_count - i - 1;
    };
    for(int i = 0; i < (int)RHIBindlessResourceType::kMax; i++) {
        SetupBindlessChannel((RHIBindlessResourceType)i);
    }
}

void RHIBindlessManager::AllocateResourceSlot(const RHIBindlessResourceDesc & desc, RHIBindlessResourceSlotKeeper * out_slot) {
    std::lock_guard lock(mutex_);
    auto & channel = bindless_channels_[(uint32_t)desc.type];
    mi_assert(channel.unused_count >= 1, "Not enough bindless slots");
    out_slot->slot_ = channel.unused[--channel.unused_count];
    out_slot->type_ =  desc.type;
}

void RHIBindlessManager::FreeResourceSlot(RHIBindlessResourceSlotKeeper * slot) {
    std::lock_guard lock(mutex_);
    auto & channel = bindless_channels_[(uint32_t)slot->type_];
    channel.unused[channel.unused_count++] = slot->slot_;
}


MI_NAMESPACE_END



