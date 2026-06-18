/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "meshing/chunk_streamer.h"
#include "meshing/chunk_meshing_context.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

MACROMC_MESHING_NAMESPACE_BEGIN

ChunkStreamer::ChunkStreamer(Config config) : config_(std::move(config)) {}

mi::TRef<ChunkStreamer> ChunkStreamer::Create(Config config) {
    return mi::TRef<ChunkStreamer>(new ChunkStreamer(std::move(config)));
}

void ChunkStreamer::SetRegistry(ChunkRegistry* registry) { registry_ = registry; }
void ChunkStreamer::SetMeshingContext(ChunkMeshingContext* meshing) { meshing_ = meshing; }

void ChunkStreamer::Tick(ChunkCoord cam_chunk) {
    if (!registry_) return;

    // 1. Reconcile the envelope with the player's chunk. No-op if the player
    //    hasn't crossed a chunk border since last tick.
    UpdateEnvelope(cam_chunk);

    // 2. Flush grace-expired unloads from previous ticks.
    FlushGraceQueue();

    // 3. Harvest finished async worldgen; mark newly-ready chunks + their
    //    resident neighbours dirty so S2 will (re-)mesh them. The neighbour
    //    marking eliminates conservative boundary faces that were kept while
    //    the neighbour was still loading (neighbour-unknown => air).
    auto newly_ready = registry_->Advance();
    for (const auto& coord : newly_ready) {
        dirty_set_.insert(coord);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dz == 0) continue;
                ChunkCoord nc{coord.x + dx, coord.z + dz};
                // Only neighbours already resident need a re-mesh; absent ones
                // will be meshed fresh when they load.
                if (registry_->Find(nc)) dirty_set_.insert(nc);
            }
        }
    }
}

std::unordered_set<ChunkCoord, ChunkCoordHash> ChunkStreamer::ConsumeDirtySet() {
    return std::move(dirty_set_);
}

void ChunkStreamer::UpdateEnvelope(ChunkCoord cam_chunk) {
    // The envelope is a function of the camera chunk only. If the player hasn't
    // crossed a chunk border since last tick, the target set is identical and
    // there is nothing to reconcile — skip the rebuild entirely.
    if (envelope_cam_chunk_ && *envelope_cam_chunk_ == cam_chunk) return;

    // Build this tick's target envelope.
    // Envelope = chebyshev ball of radius (active_radius+1) in the XZ plane:
    //   - inner ball radius active_radius      => kActive
    //   - ring at exactly active_radius+1      => kBorderline (neighbour ring)
    const int R = config_.active_radius;
    std::unordered_set<ChunkCoord, ChunkCoordHash> new_envelope;
    for (int dz = -(R + 1); dz <= (R + 1); ++dz) {
        for (int dx = -(R + 1); dx <= (R + 1); ++dx) {
            new_envelope.insert({cam_chunk.x + dx, cam_chunk.z + dz});
        }
    }

    auto now = std::chrono::steady_clock::now();

    // Entries: in new_envelope but not in the old envelope_. Request load +
    // reconcile sim, and cancel any pending unload (player re-entered).
    //
    // Generation priority is distance-based: nearest chunks generate first so
    // the player sees terrain appear near->far. We map chebyshev distance
    // [0 .. R+1] into a priority window [kLow .. kGenPrioCeil], where
    // kGenPrioCeil sits just BELOW the mesh task priority range. Mesh tasks use
    // ChunkMeshingContext::ComputePriority which returns up to
    // (max_priority_distance - 1) == 127 by default; by keeping worldgen <= 127
    // as well, a near mesh task (127) never loses to a far worldgen task, and a
    // near worldgen task (127) ties with a near mesh task — mesh is what the
    // player ultimately waits on. The registry is unaware of the player/camera;
    // the streamer computes the priority and hands it over.
    constexpr mi::TaskPriority kGenPrioFloor = mi::TaskPriorities::kLow;   // farthest
    constexpr mi::TaskPriority kGenPrioCeil  = 127;                          // nearest (<= mesh max)
    const int max_cheb = R + 1;
    for (const auto& c : new_envelope) {
        if (envelope_.count(c)) continue;  // was already inside

        int cheb = std::max(std::abs(c.x - cam_chunk.x), std::abs(c.z - cam_chunk.z));
        ChunkSim target_sim = (cheb <= R) ? ChunkSim::kActive : ChunkSim::kBorderline;

        // Linear interp: cheb=0 -> ceil, cheb=max_cheb -> floor. Use a uint64
        // intermediate to avoid uint32 overflow (the priority range is
        // ~UINT32_MAX/4 wide; scaling by cheb could overflow uint32).
        mi::TaskPriority span = kGenPrioCeil - kGenPrioFloor;
        mi::TaskPriority gen_prio = static_cast<mi::TaskPriority>(
            kGenPrioCeil - span * static_cast<uint64_t>(cheb) / max_cheb);

        if (registry_->Has(c)) {
            if (registry_->GetSimState(c) != target_sim) {
                registry_->SetSimState(c, target_sim);
            }
        } else {
            registry_->RequestLoad(c, config_.shell_category, target_sim, gen_prio);
        }
        pending_unloads_.erase(c);  // player re-entered; cancel deferred unload
    }

    // Exits: in the old envelope_ but not in new_envelope. Queue them for
    // graceful unload. This catches exits even for chunks still kLoading
    // (which a registry ready-snapshot scan would miss -> orphan chunks).
    for (const auto& c : envelope_) {
        if (new_envelope.count(c)) continue;  // still inside
        if (!pending_unloads_.count(c)) {     // don't reset an existing timer
            pending_unloads_[c] = now;
        }
    }

    envelope_ = std::move(new_envelope);
    envelope_cam_chunk_ = cam_chunk;
}

void ChunkStreamer::FlushGraceQueue() {
    if (!registry_ || pending_unloads_.empty()) return;
    auto now = std::chrono::steady_clock::now();
    auto grace = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(config_.unload_grace_seconds));
    for (auto it = pending_unloads_.begin(); it != pending_unloads_.end();) {
        if ((now - it->second) >= grace) {
            // Tear down meshing-side state before releasing the registry entry,
            // so the meshing context drops its TRef lease and cancels any
            // in-flight mesh task on this chunk.
            if (meshing_) meshing_->UnregisterChunk(it->first);
            dirty_set_.erase(it->first);
            registry_->RequestUnload(it->first);
            it = pending_unloads_.erase(it);
        } else {
            ++it;
        }
    }
}

MACROMC_MESHING_NAMESPACE_END
