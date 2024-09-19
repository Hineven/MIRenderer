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
#include "rhi/rhi_bindlesskeeper.h"


MI_NAMESPACE_BEGIN

RHIBindlessSlotKeeperBase::~RHIBindlessSlotKeeperBase() {
    RHI::Get().GetBindlessManager().FreeResourceSlot(this);
}

void RHIBindlessSlotKeeperBase::CommitChanges(uint32_t offset, int size) const {
    mi_assert(offset >= 0 && size > 0 && offset + size <= num_slots_, "Invalid offset or size");
    RHI::Get().GetBindlessManager().CommitResourceSlotUpdate(type_, slot_ + offset, size);
}

RHIBindlessSlotKeeperBase::RHIBindlessSlotKeeperBase() {

}

RHIResource * RHIBindlessSlotKeeperBase::Get_Impl(uint32_t offset) {
    auto & mgr = RHI::Get().GetBindlessManager();
    auto ptr = mgr.bindless_channels_[(size_t)type_].resource_refs[slot_ + offset].Raw();
    return ptr;
}

void RHIBindlessSlotKeeperBase::Set_Impl(RHIResource *resource, uint32_t offset) {
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.bindless_channels_[(size_t)type_].resource_refs[slot_ + offset] = resource;
}

void RHIBindlessSlotKeeperBase::Commit_Impl(uint32_t offset) {
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.CommitResourceSlotUpdate(type_, slot_ + offset);
}

void RHIBindlessSlotKeeper<RHIBuffer>::Set(RHIBuffer *resource, uint32_t offset) {
    mi_assert(offset < num_slots_, "Offset exceeds the number of slots");
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.bindless_channels_[(size_t)type_].resource_refs[slot_ + offset] = (RHIResource*)resource;
    if(type_ == RHIBindlessResourceType::kStorageBuffer) {
        mgr.bindless_buffer_channel[0].offsets[slot_ + offset] = 0;
        mgr.bindless_buffer_channel[0].sizes[slot_ + offset] = (size_t)-1ll;
    } else if(type_ == RHIBindlessResourceType::kUniformBuffer) {
        mgr.bindless_buffer_channel[1].offsets[slot_ + offset] = 0;
        mgr.bindless_buffer_channel[1].sizes[slot_ + offset] = (size_t)-1ll;
    }
}

void RHIBindlessSlotKeeper<RHIBuffer>::SetAndCommit(RHIBuffer *resource, uint32_t offset) {
    Set(resource, offset);
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.CommitResourceSlotUpdate(type_, slot_ + offset);
}

RHIBufferSpan RHIBindlessSlotKeeper<RHIBufferSpan>::Get(uint32_t offset) {
    mi_assert(offset < num_slots_, "Offset exceeds the number of slots");
    auto & mgr = RHI::Get().GetBindlessManager();
    auto ptr = mgr.bindless_channels_[(size_t)type_].resource_refs[slot_ + offset].Raw();
    return RHIBufferSpan((RHIBuffer*) ptr, mgr.bindless_buffer_channel[(size_t)type_].offsets[slot_ + offset], mgr.bindless_buffer_channel[(size_t)type_].sizes[slot_ + offset]);
}

void RHIBindlessSlotKeeper<RHIBufferSpan>::Set(RHIBufferSpan resource, uint32_t offset) {
    mi_assert(offset < num_slots_, "Offset exceeds the number of slots");
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.bindless_channels_[(size_t)type_].resource_refs[slot_ + offset] = (RHIResource*)resource.buffer;
    mgr.bindless_buffer_channel[(size_t)type_].offsets[slot_ + offset] = resource.offset;
    mgr.bindless_buffer_channel[(size_t)type_].sizes[slot_ + offset] = resource.size;
}

void RHIBindlessSlotKeeper<RHIBufferSpan>::SetAndCommit(RHIBufferSpan resource, uint32_t offset) {
    Set(resource, offset);
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.CommitResourceSlotUpdate(type_, slot_ + offset);
}

void RHIBindlessManager::CommitResourceSlotUpdate(RHIBindlessSlotKeeperBase *slot) {
    CommitResourceSlotUpdate(slot->type_, slot->slot_, slot->num_slots_);
}

void RHIBindlessManager::CommitResourceSlotUpdate(RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) {
    // Fallthrough, nothing we need to do.
    CommitResourceSlotUpdateRHI(type, slot, num_slots);
}

RHIBindlessManager::RHIBindlessManager() {
    auto support = RHI::Get().QueryRHIBindlessSupportInfo();
    auto SetupBindlessChannel = [&](RHIBindlessResourceType channel_type) {
        auto & channel = bindless_channels_[(uint32_t)channel_type];
        uint32_t limit = 0;
        if(channel_type == RHIBindlessResourceType::kSampler)
            limit = support.max_num_sampler_slots;
        else if(channel_type == RHIBindlessResourceType::kAccelerationStructure)
            limit = std::min(support.max_num_resource_slots, C::kMaxNumBindlessAccelerationStructures);
        else limit = support.max_num_resource_slots;
        limit = std::min(limit, C::kMaxNumBindlessResourceSlotsPerChannel);
        channel.size   = limit;
        channel.unused_count = limit;
        for(int i = 0; i < channel.unused_count; i++)
            channel.unused[i] = channel.unused_count - i - 1;
    };
    for(int i = 0; i < (int)RHIBindlessResourceType::kMaxAndImmSampler; i++) {
        SetupBindlessChannel((RHIBindlessResourceType)i);
    }
}

void RHIBindlessManager::AllocateResourceSlot(const RHIBindlessResourceDesc & desc, RHIBindlessSlotKeeperBase * out_slot) {
    auto & channel = bindless_channels_[(uint32_t)desc.type];
    mi_assert(channel.unused_count >= desc.num_slots, "Not enough bindless slots");
    mi_assert(desc.num_slots > 0, "Invalid number of slots");
    if(desc.num_slots == 1) {
        int candidate = channel.unused[--channel.unused_count];
        // Find a slot that is not used
        while(channel.unused_count && channel.num_slots[candidate] != 0) {
            candidate = channel.unused[--channel.unused_count];
        }
        mi_assert(channel.num_slots[candidate] == 0, "Not enough bindless slots");
        out_slot->slot_ = candidate;
        out_slot->type_ = desc.type;
        out_slot->num_slots_ = desc.num_slots;
        channel.num_slots[out_slot->slot_] = 1;
    } else {
        out_slot->slot_ = (uint32_t)-1;
        int empty_count = 0;
        for(int i = 0; i < channel.size; i++) {
            if(channel.num_slots[i] == 0) {
                empty_count++;
                if(empty_count == desc.num_slots) {
                    // Found a segment of empty slots, allocate
                    out_slot->slot_ = i - empty_count + 1;
                    out_slot->type_ = desc.type;
                    out_slot->num_slots_ = desc.num_slots;
                    channel.num_slots[out_slot->slot_] = desc.num_slots;
                    for(int j = 1; j < desc.num_slots; j++) {
                        channel.num_slots[i - empty_count + 1 + j] = -1;
                    }
                    break;
                }
            } else {
                empty_count = 0;
            }
        }
        mi_assert(out_slot->slot_ != (uint32_t)-1, "Not enough bindless slots");
    }
}

void RHIBindlessManager::FreeResourceSlot(RHIBindlessSlotKeeperBase * keeper) {
    auto & channel = bindless_channels_[(uint32_t)keeper->type_];
    for(int i = 0; i < (int)keeper->num_slots_; i++) {
        int slot = (int)keeper->slot_ + i;
        channel.unused[channel.unused_count++] = slot;
        channel.num_slots[slot] = 0;
    }
    FreeResourceSlotRHI(keeper->type_, keeper->slot_, keeper->num_slots_);
}


MI_NAMESPACE_END




