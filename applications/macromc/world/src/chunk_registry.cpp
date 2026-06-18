/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/chunk_registry.h"
#include "world/chunk_coord.h"
#include "profiler/profiler.h"

#include <atomic>
#include <memory>
#include <unordered_set>
#include <utility>

MACROMC_WORLD_NAMESPACE_BEGIN

ChunkRegistry::ChunkRegistry() = default;

ChunkRegistry::~ChunkRegistry() {
    // Cancel any still-in-flight generation before teardown. TaskGroups hold
    // TRefs to their tasks; cancelling lets running workers exit cleanly. The
    // registry does not own chunk data (WorldShellData does); the Entry's
    // non-owning ChunkData* pointers simply go away with the map.
    for (auto& [coord, entry] : chunks_) {
        if (entry.gen_task_group) entry.gen_task_group->CancelAll();
    }
}

void ChunkRegistry::RegisterGenerator(ShellCategory category, ChunkGeneratorFn* generator) {
    generators_[static_cast<size_t>(category)] = generator;
}

void ChunkRegistry::SetChunkInstallSink(ChunkInstallSink sink) {
    install_sink_ = std::move(sink);
}

void ChunkRegistry::SetChunkRemoveSink(ChunkRemoveSink sink) {
    remove_sink_ = std::move(sink);
}

void ChunkRegistry::LaunchGeneration(Entry& entry, const ChunkCoord& coord,
                                     mi::TaskPriority priority) {
    ChunkGeneratorFn* gen = generators_[static_cast<size_t>(entry.pending_category)];
    // No generator registered for this category: leave presence at kAbsent.
    // The chunk will simply never become ready. (Upper layer should ensure a
    // generator exists before requesting load for a category.)
    if (!gen || !*gen) return;

    entry.gen_task_group = mi::Create<mi::TaskGroup>();
    entry.gen_handle.result_ptr = std::make_shared<mi::TRef<ChunkData>>(mi::TRef<ChunkData>{});
    entry.gen_handle.done_flag = std::make_shared<std::atomic<bool>>(false);
    entry.presence = ChunkPresence::kLoading;

    // The worker creates the ChunkData, fills it via the injected generator,
    // and stashes the result into the shared slot. Advance() harvests it via
    // the done_flag. We capture the generator by copy (it captures worldgen
    // state by ref, safe because the app outlives the registry) and the token
    // so long generations can cooperatively cancel.
    auto token = entry.gen_task_group->GetToken();
    ChunkGeneratorFn gen_copy = *gen;
    auto result = entry.gen_handle.result_ptr;
    auto done_flag = entry.gen_handle.done_flag;

    auto body = [coord, gen_copy = std::move(gen_copy), token, result, done_flag]() {
        if (token->IsCancelled()) return;
        // Time a single chunk generation. Null-safe: no-op when no global profiler.
        ::macromc::Profiler::ScopeHandle gen_scope =
            []() -> ::macromc::Profiler::ScopeHandle {
                auto* p = ::macromc::Profiler::GetGlobal();
                return p ? p->Scope("worldgen.single") : ::macromc::Profiler::ScopeHandle{};
            }();
        auto chunk = mi::Create<ChunkData>(coord);
        gen_copy(coord, chunk.Raw());
        if (token->IsCancelled()) return;
        *result = std::move(chunk);
        done_flag->store(true, std::memory_order_release);
    };

    // Build, attach to the group, then enqueue. Done(false) + BatchEnqueue
    // follows the BatchEnqueue contract ("tasks must not have been fired yet").
    auto task = mi::TaskGraph::Get().CreateTask(std::move(body))
                    .SetPriority(priority)
                    .Done(false);
    entry.gen_task_group->Add(task);
    mi::TaskGraph::Get().BatchEnqueue({task});
}

void ChunkRegistry::RequestLoad(const ChunkCoord& coord, ShellCategory category,
                                ChunkSim initial_sim, mi::TaskPriority priority) {
    auto it = chunks_.find(coord);
    if (it != chunks_.end()) {
        // Already known: just bump sim if requested.
        if (initial_sim != ChunkSim::kInactive) {
            it->second.sim = initial_sim;
        }
        return;
    }

    Entry entry;
    entry.pending_category = category;
    entry.sim = initial_sim;
    entry.pending_sim = initial_sim;
    auto [inserted_it, _] = chunks_.emplace(coord, std::move(entry));
    LaunchGeneration(inserted_it->second, coord, priority);
}

void ChunkRegistry::RequestUnload(const ChunkCoord& coord) {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return;

    // Cancel any in-flight generation first (cooperative: running workers exit).
    if (it->second.gen_task_group) {
        it->second.gen_task_group->CancelAll();
    }
    // Notify the owning layer to drop its owning TRef (the registry never owned
    // the chunk data itself). Any external TRef leases (e.g. held by the meshing
    // context) keep the data alive until they too release.
    if (it->second.chunk && remove_sink_) {
        remove_sink_(coord);
    }
    it->second.presence = ChunkPresence::kUnloading;
    it->second.chunk = nullptr;
    it->second.gen_task_group = nullptr;
    it->second.gen_handle = {};
    chunks_.erase(it);
}

void ChunkRegistry::SetSimState(const ChunkCoord& coord, ChunkSim sim) {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return;
    it->second.sim = sim;
    // If still loading, also record as pending so Advance() applies it.
    if (it->second.presence == ChunkPresence::kLoading) {
        it->second.pending_sim = sim;
    }
}

std::vector<ChunkCoord> ChunkRegistry::Advance() {
    std::vector<ChunkCoord> newly_ready;
    for (auto& [coord, entry] : chunks_) {
        if (entry.presence != ChunkPresence::kLoading) continue;
        if (!entry.gen_handle.done_flag) continue;

        // Non-blocking completion check: the generation worker sets done_flag
        // (release) after writing the result slot.
        if (!entry.gen_handle.done_flag->load(std::memory_order_acquire)) continue;

        // Cancelled generation (token fired mid-run): drop to kAbsent and let a
        // future RequestLoad retry. done_flag may be true even on cancel if the
        // worker set it before observing the token, so we accept whatever landed.
        if (entry.gen_handle.result_ptr && *entry.gen_handle.result_ptr) {
            // Hand the owning TRef to the install sink (upper layer stores it in
            // the owning container, e.g. WorldShellData) and keep the returned
            // non-owning pointer for state queries. Without a sink the data has
            // nowhere to live, so we cannot mark the chunk ready.
            if (install_sink_) {
                entry.chunk = install_sink_(coord, entry.pending_category,
                                            std::move(*entry.gen_handle.result_ptr));
                entry.presence = entry.chunk ? ChunkPresence::kReady : ChunkPresence::kAbsent;
            } else {
                // No sink: the generated data would be dropped immediately. Leave
                // the chunk absent rather than hold a dangling reference.
                entry.presence = ChunkPresence::kAbsent;
            }
            // Apply any sim that was requested while loading (only if ready).
            if (entry.presence == ChunkPresence::kReady) {
                entry.sim = entry.pending_sim;
                newly_ready.push_back(coord);
            }
        } else {
            // Generation produced nothing (cancelled before writing). Reset so
            // a later RequestLoad can re-launch.
            entry.presence = ChunkPresence::kAbsent;
        }

        // Clear the handle + task group either way — shared_ptr refs and the
        // group's TRef drop here.
        entry.gen_task_group = nullptr;
        entry.gen_handle = {};
    }
    // Counter: chunks that became ready this tick. Useful as a rate
    // (chunks/s) to gauge worldgen throughput under streaming load.
    if (!newly_ready.empty()) {
        MI_PROF_INCREMENT(worldgen_chunks_ready, newly_ready.size());
    }
    return newly_ready;
}

ChunkData* ChunkRegistry::Find(const ChunkCoord& coord) const {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return nullptr;
    if (it->second.presence != ChunkPresence::kReady) return nullptr;
    return it->second.chunk;
}

ChunkPresence ChunkRegistry::GetPresence(const ChunkCoord& coord) const {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return ChunkPresence::kAbsent;
    return it->second.presence;
}

ChunkSim ChunkRegistry::GetSimState(const ChunkCoord& coord) const {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return ChunkSim::kInactive;
    return it->second.sim;
}

bool ChunkRegistry::Has(const ChunkCoord& coord) const {
    return chunks_.find(coord) != chunks_.end();
}

size_t ChunkRegistry::GetEntryCount() const {
    return chunks_.size();
}

ChunkRegistry::ActiveSnapshot ChunkRegistry::GetActiveSnapshot() const {
    ActiveSnapshot snap;
    for (const auto& [coord, entry] : chunks_) {
        if (entry.presence == ChunkPresence::kReady && entry.sim == ChunkSim::kActive) {
            snap.chunks.emplace_back(coord, entry.chunk);
        }
    }
    return snap;
}

ChunkRegistry::ReadySnapshot ChunkRegistry::GetReadySnapshot() const {
    ReadySnapshot snap;
    for (const auto& [coord, entry] : chunks_) {
        if (entry.presence == ChunkPresence::kReady) {
            snap.chunks.emplace_back(coord, entry.chunk);
        }
    }
    return snap;
}

std::vector<ChunkCoord> ChunkRegistry::GetResidentRegions() const {
    // Collect the distinct region coords that still contain at least one entry.
    // Cheap relative to a tick: one pass over chunks_ + a hash set dedupe.
    std::unordered_set<ChunkCoord, ChunkCoordHash> seen;
    seen.reserve(chunks_.size());
    for (const auto& [coord, entry] : chunks_) {
        (void)entry;
        seen.insert(ChunkToRegionCoord(coord));
    }
    return std::vector<ChunkCoord>(seen.begin(), seen.end());
}

MACROMC_WORLD_NAMESPACE_END
