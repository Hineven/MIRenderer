/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/event_bus.h"
#include "world/chunk_coord.h"

#include <chrono>
#include <memory>

MACROMC_WORLD_NAMESPACE_BEGIN

bool EventBus::Emit(const Event& e) {
    ChunkCoord region = ChunkToRegionCoord(e.target_chunk);
    auto it = inboxes_.find(region);
    if (it == inboxes_.end()) {
        // Lazy-create the region inbox on first emit. The inbox is non-movable
        // (TConsumeAllQueue owns a shared_mutex + fixed array), so it lives
        // behind a unique_ptr that stays put during map rehashes.
        InboxSlot slot;
        slot.queue = std::make_unique<Inbox>();
        it = inboxes_.emplace(region, std::move(slot)).first;
    }
    return it->second.queue->Push(e);
}

std::vector<Event> EventBus::DrainAll() {
    std::vector<Event> all;
    for (auto& [region, slot] : inboxes_) {
        auto bucket = slot.queue->ConsumeAll();
        if (all.empty()) {
            all = std::move(bucket);
        } else {
            all.reserve(all.size() + bucket.size());
            for (auto& e : bucket) all.push_back(std::move(e));
        }
    }
    return all;
}

void EventBus::GC(const std::vector<ChunkCoord>& live_regions) {
    // Build a live-region lookup set so this stays O(live + inboxes).
    std::unordered_map<ChunkCoord, bool, ChunkCoordHash> live_set;
    live_set.reserve(live_regions.size());
    for (const auto& r : live_regions) live_set[r] = true;

    auto now = std::chrono::steady_clock::now();
    auto grace = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(kRetireGraceSeconds));

    for (auto it = inboxes_.begin(); it != inboxes_.end();) {
        bool live = live_set.find(it->first) != live_set.end();
        if (live) {
            // Region is resident: cancel any pending retirement.
            it->second.retire_at.reset();
            ++it;
            continue;
        }
        // Region is empty: start or honour the retirement timer.
        if (!it->second.retire_at) {
            it->second.retire_at = now + grace;
            ++it;
        } else if (now >= *it->second.retire_at) {
            it = inboxes_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t EventBus::GetInboxCount() const {
    return inboxes_.size();
}

MACROMC_WORLD_NAMESPACE_END
