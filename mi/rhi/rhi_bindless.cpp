/*
 * Created: 2024/7/10
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <span>
#include <ranges>
#include "include/rhi/rhi_bindless.h"
#include "core/infra.h"
#include "core/constants.h"
#include "rhi/rhi.h"
#include "rhi/rhi_bindlesskeeper.h"

// Bindless resources are read only

MI_NAMESPACE_BEGIN

RHIBindlessSlotKeeperBase::~RHIBindlessSlotKeeperBase() {
    RHI::Get().GetBindlessManager().FreeResourceSlot_Delayed(this);
}

void RHIBindlessSlotKeeperBase::Commit() const {
    RHI::Get().GetBindlessManager().CommitResourceSlotUpdate(type_, slot_);
}

RHIBindlessSlotKeeperBase::RHIBindlessSlotKeeperBase() {

}

RHIResource * RHIBindlessSlotKeeperBase::Get_Impl() {
    auto & mgr = RHI::Get().GetBindlessManager();
    auto ptr = mgr.bindless_channels_[(size_t)type_].resource_refs[slot_].Raw();
    return ptr;
}

void RHIBindlessSlotKeeperBase::Set_Impl(RHIResource *resource) {
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.bindless_channels_[(size_t)type_].resource_refs[slot_] = resource;
}

void RHIBindlessSlotKeeperBase::Commit_Impl() {
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.CommitResourceSlotUpdate(type_, slot_);
}

void RHIBindlessSlotKeeper<RHIBuffer>::Set(RHIBuffer *resource) {
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.bindless_channels_[(size_t)type_].resource_refs[slot_] = (RHIResource*)resource;
}

void RHIBindlessSlotKeeper<RHIBuffer>::SetAndCommit(RHIBuffer *resource) {
    Set(resource);
    auto & mgr = RHI::Get().GetBindlessManager();
    mgr.CommitResourceSlotUpdate(type_, slot_);
}

void RHIBindlessManager::CommitResourceSlotUpdate(RHIBindlessSlotKeeperBase *slot) {
    CommitResourceSlotUpdateRHI(slot->type_, slot->slot_, 1);
}

void RHIBindlessManager::CommitResourceSlotUpdate(RHIBindlessResourceType type, uint32_t slot, uint32_t num_slots) {
    CommitResourceSlotUpdateRHI(type, slot, num_slots);
}

void RHIBindlessManager::CommitResourceSlotUpdate(RHIBindlessResourceType type, std::span<const uint32_t> slots) {
    // Go in different paths
    uint32_t min_slot = std::ranges::min(slots);
    uint32_t max_slot = std::ranges::max(slots);
    if (max_slot - min_slot <= slots.size() * 1.5) {
        CommitResourceSlotUpdateRHI(type, min_slot, max_slot - min_slot + 1);
    } else {
        if (slots.size() <= 4) {
            for (auto e : slots) {
                CommitResourceSlotUpdateRHI(type, e, 1);
            }
        } else {
            // Try to segment the commits with herustics
            std::vector<uint32_t> v(slots.size());
            std::copy(slots.begin(), slots.end(), v.begin());
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
            uint32_t first_index = 0;
            uint32_t max_whitespace_size = std::max(16u, (uint32_t)((float)slots.size() * 0.1));
            for (size_t i = 1; i < v.size(); i++) {
                uint32_t diff = v[i] - v[i - 1];
                if (diff > max_whitespace_size) {
                    CommitResourceSlotUpdateRHI(type, v[first_index], v[i - 1] - v[first_index] + 1);
                    first_index = (uint32_t)i;
                }
            }
            CommitResourceSlotUpdateRHI(type, v[first_index], v.back() - v.front() + 1);
        }
    }
}

RHIBindlessManager::RHIBindlessManager() {
    auto support = RHI::Get().QueryRHIBindlessSupportInfo();
    auto SetupBindlessChannel = [&](RHIBindlessResourceType channel_type) {
        auto & channel = bindless_channels_[(uint32_t)channel_type];
        uint32_t limit = 0;
        if(channel_type == RHIBindlessResourceType::kAccelerationStructure)
            limit = std::min(support.max_num_resource_slots, C::kMaxNumBindlessAccelerationStructures);
        else limit = support.max_num_resource_slots;
        limit = std::min(limit, C::kMaxNumBindlessResourceSlotsPerChannel);
        channel.total_count  = limit;
        channel.unused_count = limit;
        for(uint32_t i = 0; i < channel.unused_count; i++)
            channel.unused[i] = channel.unused_count - i - 1;
    };
    for(int i = 0; i < (int)RHIBindlessResourceType::kMax; i++) {
        SetupBindlessChannel((RHIBindlessResourceType)i);
    }
}

void RHIBindlessManager::AllocateResourceSlot(const RHIBindlessResourceDesc & desc, RHIBindlessSlotKeeperBase * out_slot) {
    auto & channel = bindless_channels_[(uint32_t)desc.type];
    mi_assert(channel.unused_count > 0, "Not enough bindless slots");
    int candidate = channel.unused[--channel.unused_count];
    out_slot->slot_ = candidate;
    out_slot->type_ = desc.type;

    // Remove the slot from delayed free slots if present
    auto it = delayed_free_slots_.find(RHIPackedBindlessSlot::Pack(desc.type, candidate));
    if (it != delayed_free_slots_.end()) {
        delayed_free_slots_.erase(it);
    }
}

void RHIBindlessManager::FreeResourceSlotFromTable(RHIBindlessSlotKeeperBase *keeper) {
    auto & channel = bindless_channels_[(uint32_t)keeper->type_];
    int slot = (int)keeper->slot_;
    channel.unused[channel.unused_count++] = slot;
}


void RHIBindlessManager::FreeResourceSlot(RHIBindlessSlotKeeperBase * keeper) {
    FreeResourceSlotFromTable(keeper);
    FreeResourceSlotRHI(keeper->type_, keeper->slot_, 1);
}

// Free the resource slot, but RHI invocation is batched to descriptor swapping.
void RHIBindlessManager::FreeResourceSlot_Delayed(RHIBindlessSlotKeeperBase *keeper) {
    FreeResourceSlotFromTable(keeper);
    delayed_free_slots_.insert(RHIPackedBindlessSlot::Pack(keeper->type_, keeper->slot_));
}

std::span<RHIPackedBindlessSlot> RHIBindlessManager::PrepareDelayedSlotsForRHIFree() {
    if (delayed_free_slots_.size() == 0) {
        return {};
    }
    auto rhi_delayed_slots = new RHIPackedBindlessSlot[delayed_free_slots_.size()];
    auto dst_span = std::span(rhi_delayed_slots, delayed_free_slots_.size());
    for (auto [src, dst] : std::views::zip(delayed_free_slots_, dst_span)) {
        dst = src;
    }
    delayed_free_slots_.clear();
    return dst_span; // NOLINT
}

void RHIBindlessManager::PreDestruction() {
    for (auto & chan : bindless_channels_) {
        for (auto & e : chan.resource_refs) {
            if (e && e->GetRefCount() != 1) {
                MI_WARN("BindlessManager: Resource {} is not released outside of the bindless manager "
                        "before RHI destruction", (void*)e.Raw());
            }
            e.SafeRelease();
        }
    }
}



MI_NAMESPACE_END




