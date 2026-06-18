/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_EVENT_BUS_H
#define MACROMC_WORLD_EVENT_BUS_H

#include "world/common.h"
#include "world/types.h"
#include "world/block_data.h"
#include "world/chunk_data.h"
#include "world/shell_data.h"
#include "core/refcounted.h"
#include "core/util/queue.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>
#include <unordered_map>

MACROMC_WORLD_NAMESPACE_BEGIN

// =============================================================================
// EventBus — cross-tick, per-region event delivery.
//
// This realises the "Events" data line of TICK_PACING.md §3.2. Events carry
// cross-chunk far-field interactions (beyond the 15-block same-tick rule) and
// the S4 loose-physics results that must fold back into gameplay next tick.
//
// Routing granularity: per-REGION inbox (one region = kRegionSize x kRegionSize
// chunks in the XZ plane; see chunk_coord.h). A region is the checkerboard
// scheduling work unit (8x8 chunks), so events land in the same inbox as the
// chunks that will consume them. This is the middle ground decided in design:
// cheaper than per-chunk (×N mailboxes), more structured than a single global
// queue (drained output is naturally grouped by region for S1 dispatch).
//
// Timing contract (TICK_PACING §3.2 "Events" row):
//   - S1 start (after sync A): single-thread DrainAll() — one ConsumeAll per
//     region inbox, flattened into a vector grouped by target_chunk for the
//     checkerboard consumers. O(total events), single-threaded, negligible.
//   - During S1 (checkerboard parallel) and during S4 (cross-tick): any thread
//     may Emit() into a region inbox. TConsumeAllQueue is MPSC (shared_lock
//     Push), so concurrent producers are safe with the single S1 consumer.
//
// The queue is append-only and drained wholesale each tick — no per-event
// lifetime, no mid-tick partial consumption.
// =============================================================================

// --- Event payloads. Add new variants here AND to EventPayload below. ---
// Cross-chunk block mutation (e.g. far-field redstone, a teleport-placed block).
struct BlockUpdateEvent {
    glm::ivec3 world_pos;   // target world block position
    BlockData block;        // new block identity
};
// S4 loose-physics contact result folding back into S1 gameplay.
struct PhysicsContactEvent {
    uint64_t entity_a;      // placeholder IDs until the entity system exists
    uint64_t entity_b;
    glm::vec3 contact_point_ws;
};
// Cross-owner inventory operation (see TICK_PACING §3.7). Player A touching
// player B's chest emits this to B's owning chunk; B consumes serially in S1.
struct InventoryOpEvent {
    uint64_t source_owner;  // who initiated (entity/chunk id)
    uint64_t target_owner;  // who owns the inventory being touched
    uint32_t slot;
    uint32_t count;
    uint16_t op;            // op code (place/take/swap...) — filled later
};
// A chunk finished loading and became kReady (registry emits; consumers may
// use it to trigger dependent work).
struct ChunkLoadedEvent {
    ChunkCoord coord;
};

// Closed set of event types. std::variant gives compile-time exhaustiveness
// (std::visit), no heap allocation per event, no virtual dispatch.
using EventPayload = std::variant<
    BlockUpdateEvent,
    PhysicsContactEvent,
    InventoryOpEvent,
    ChunkLoadedEvent>;

// A single event: where it goes (target_chunk) + what it carries.
struct Event {
    ChunkCoord target_chunk;   // decides which region inbox it lands in
    EventPayload payload;
};

// =============================================================================
// EventBus
//
// Two-tier model (the second tier is a planned extension, see TODO below):
//   - Tier 1 (this class): per-region inboxes for INSTANT events — events whose
//     target is in the active set and will be consumed this tick. DrainAll()
//     flushes them wholesale every S1 start. No cross-tick accumulation here.
//   - Tier 2 (planned): per-chunk PERSISTENT inboxes for events whose target is
//     not currently loaded / not simulating (e.g. a piped item sent to an
//     unloaded chunk's inventory). Tier-1 dispatch redirects such events to the
//     target chunk's persistent inbox, which accumulates across ticks and is
//     serialized with the chunk — so nothing is silently dropped across a load
//     boundary. Until Tier 2 lands, events whose target has no consumer are
//     dropped at dispatch (a known limitation; see DrainAll note).
// =============================================================================
class EventBus : public mi::RefCounted<> {
public:
    EventBus() = default;
    ~EventBus() override = default;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Emit an event from any thread (S1 checkerboard workers, S4 physics).
    // Routes to the region inbox of target_chunk (tier 1). Returns false if
    // that inbox is full (TConsumeAllQueue budget exceeded) — caller may retry/drop.
    bool Emit(const Event& e);

    // Drain ALL region inboxes. Called once at S1 start (single-threaded, after
    // sync A). Returns events flattened into a vector; consumers dispatch by
    // target_chunk. Each inbox is ConsumeAll'd (reset to empty) atomically.
    //
    // NOTE (tier-2 limitation): events whose target_chunk has no registered
    // consumer (chunk not loaded / not simulating) are returned here but have
    // nowhere to go — the caller currently drops them. Once the per-chunk
    // persistent tier lands, the dispatcher should redirect these to the
    // target's persistent inbox instead of dropping. See class header TODO.
    std::vector<Event> DrainAll();

    // Garbage-collect region inboxes whose region no longer has any resident
    // chunk — but with a grace period (kRetireGraceSeconds) to avoid churn: an
    // inbox whose region went empty is first marked for retirement (with a
    // retire-at timestamp) and only actually dropped once the grace elapses AND
    // the region is still empty. If the region becomes resident again within
    // the grace window, the retirement is cancelled. This prevents the inbox
    // from being torn down and immediately rebuilt when a chunk flickers in/out
    // near a region edge, or when events keep arriving for a just-unloaded area.
    //
    // Called from S0 (single-threaded) with the resident region set from
    // ChunkRegistry::GetResidentRegions(). Safe to call every tick; uses an
    // internal steady clock.
    void GC(const std::vector<ChunkCoord>& live_regions);

    // Grace period (seconds) before an empty region's inbox is actually freed.
    static constexpr double kRetireGraceSeconds = 10.0;

    // Number of region inboxes currently allocated (diagnostic).
    size_t GetInboxCount() const;

private:
    // Per-region inbox wrapper. The queue itself is non-movable (owns a
    // shared_mutex + fixed array), so it lives behind a unique_ptr that stays
    // put during map rehashes. The optional retire_at timestamp supports
    // delayed GC (see GC): set when the region first goes empty, cleared when
    // the region becomes resident again, acted on once the grace elapses.
    using Inbox = mi::TConsumeAllQueue<Event>;
    struct InboxSlot {
        std::unique_ptr<Inbox> queue;
        std::optional<std::chrono::steady_clock::time_point> retire_at;
    };
    std::unordered_map<ChunkCoord, InboxSlot, ChunkCoordHash> inboxes_;
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_EVENT_BUS_H
