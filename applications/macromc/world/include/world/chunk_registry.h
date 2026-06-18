/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_CHUNK_REGISTRY_H
#define MACROMC_WORLD_CHUNK_REGISTRY_H

#include "world/common.h"
#include "world/types.h"
#include "world/chunk_data.h"
#include "world/shell_data.h"
#include "core/refcounted.h"
#include "core/task.h"

#include <functional>
#include <unordered_map>
#include <vector>

MACROMC_WORLD_NAMESPACE_BEGIN

// =============================================================================
// ChunkRegistry — the global chunk index + presence/sim state machine.
//
// This is the S0 (Registry) stage implementation per TICK_PACING.md. It owns:
//   - A global flat ChunkCoord -> Entry index (one map for the whole world,
//     spanning all shells). A global index is chosen over per-shell indices so
//     that chunk ownership can migrate across shells at fracture time without
//     re-keying.
//   - The dual-axis chunk state: ChunkPresence (voxel data lifecycle) and
//     ChunkSim (gameplay participation). ChunkData itself stays a pure data
//     container and carries no state — the state lives here, keyed by coord.
//
// Driving model (see TICK_PACING §3.1):
//   - Register-on-step: RequestLoad() immediately steps presence kAbsent ->
//     kLoading and launches an async generation task on the TaskGraph.
//   - Tick-driven harvest: Advance() is called once per tick in S0; it scans
//     in-flight generation tasks and installs any that finished, stepping
//     their presence kLoading -> kReady.
//
// Threading contract:
//   - All mutating methods (RequestLoad / RequestUnload / SetSimState /
//     Advance / Clear) are called ONLY from the S0 single thread.
//   - Read methods (Find / Get* / GetActiveSnapshot) may be called from S1/S2
//     readers; they are safe because sync-gate A guarantees the registry is
//     stable by the time S1 begins.
//   - The generation task bodies run on worker threads but only touch their
//     own captured output (a freshly created ChunkData); they never touch the
//     registry's map. The registry observes completion via Task::GetState().
// =============================================================================

// Generator callback injected by the upper layer (the app owns the Worldgen
// instance and wraps it into this signature, so the world module does not
// depend on the worldgen module — avoids a circular dependency).
//   coord     — chunk to generate
//   out_data  — freshly created ChunkData to fill (worker thread)
using ChunkGeneratorFn = std::function<void(const ChunkCoord& coord, ChunkData* out_data)>;

// Selects which generator to use for a given chunk, based on shell category.
// The app registers one generator per ShellCategory at construction.
using ChunkGeneratorSelector = std::function<ChunkGeneratorFn*(ShellCategory category)>;

// Ownership sink: when a chunk finishes generating, Advance() hands the owning
// TRef<ChunkData> to this callback. The upper layer (app) stores it into the
// owning container (WorldShellData) and returns a non-owning ChunkData* for the
// registry to keep as a state-side reference. This keeps the registry a pure
// state index (it does NOT own chunk data), per the ownership split where
// WorldShellData is the sole owner.
//
// Threading: invoked on the S0 (Advance) thread only.
using ChunkInstallSink = std::function<ChunkData*(const ChunkCoord& coord,
                                                  ShellCategory category,
                                                  mi::TRef<ChunkData> chunk)>;

class ChunkRegistry : public mi::RefCounted<> {
public:
    ChunkRegistry();
    ~ChunkRegistry() override;

    ChunkRegistry(const ChunkRegistry&) = delete;
    ChunkRegistry& operator=(const ChunkRegistry&) = delete;

    // --- Generator wiring (call once at startup, before any RequestLoad) ---
    // Registers a generator for a shell category. The registry does not take
    // ownership of the generator (it stores a pointer; caller must keep it alive).
    void RegisterGenerator(ShellCategory category, ChunkGeneratorFn* generator);

    // Install the ownership sink (call once at startup). When a chunk finishes
    // generating, Advance() moves the owning TRef<ChunkData> into this sink and
    // stores the returned non-owning pointer in the Entry. Without a sink
    // installed, Advance() cannot retain a reference to the generated chunk
    // (it would be dropped) — so callers that want kReady chunks must install one.
    void SetChunkInstallSink(ChunkInstallSink sink);

    // Ownership remove sink: when a chunk is unloaded (RequestUnload), the
    // registry drops its non-owning reference and calls this sink so the upper
    // layer can remove the owning TRef from its container (e.g. WorldShellData).
    // Optional — if not installed, the owning container must be cleaned up by
    // other means (the registry cannot free data it does not own).
    using ChunkRemoveSink = std::function<void(const ChunkCoord& coord)>;
    void SetChunkRemoveSink(ChunkRemoveSink sink);

    // --- Registration / deregistration (S0 single-thread) ---

    // Request a chunk be loaded. Immediately steps presence kAbsent -> kLoading
    // and launches an async generation task (register-on-step). If the chunk is
    // already present (loading or ready), this is a no-op. If sim is requested
    // active/borderline, it is recorded so Advance() can apply it on completion.
    //
    // `priority` sets the TaskGraph scheduling priority of the worldgen task.
    // The registry itself does not know about the player/camera; the caller
    // computes a distance-based priority and passes it in, so that when many
    // chunks are queued for generation the nearest ones are meshed first.
    void RequestLoad(const ChunkCoord& coord, ShellCategory category,
                     ChunkSim initial_sim = ChunkSim::kInactive,
                     mi::TaskPriority priority = mi::TaskPriorities::kNormal);

    // Request a chunk be unloaded. Steps presence -> kUnloading, cancels any
    // in-flight generation, releases the TRef, and removes the entry.
    void RequestUnload(const ChunkCoord& coord);

    // Update the sim participation of a chunk (player moved across chunk
    // borders; active-set / borderline-ring maintenance). No-op if absent.
    void SetSimState(const ChunkCoord& coord, ChunkSim sim);

    // --- Tick advance (S0 single-thread, once per tick) ---
    // Scans all kLoading entries; for each whose generation task finished,
    // installs the produced ChunkData and steps presence kLoading -> kReady
    // (applying any pending sim). Returns the list of coords that newly became
    // kReady this tick (callers use it to mark dirty for S2 derivation).
    std::vector<ChunkCoord> Advance();

    // --- Queries (any stage reader; safe after sync-gate A) ---

    // Returns the chunk's non-owning pointer if presence == kReady, else nullptr.
    // The registry does NOT own the chunk data (WorldShellData does); callers
    // that need to keep the chunk alive (e.g. the meshing context's lock-free
    // neighbour snapshots) must obtain an owning TRef lease from the owning
    // container instead. This accessor is for short-lived, same-tick reads.
    ChunkData* Find(const ChunkCoord& coord) const;
    ChunkPresence GetPresence(const ChunkCoord& coord) const;
    ChunkSim GetSimState(const ChunkCoord& coord) const;
    bool Has(const ChunkCoord& coord) const;
    size_t GetEntryCount() const;

    // Snapshot of all kActive chunks (coord + non-owning ptr), for S1
    // checkerboard scheduling. Stable for the tick (registry does not mutate
    // during S1). Callers needing ownership must lease from the owning container.
    struct ActiveSnapshot {
        std::vector<std::pair<ChunkCoord, ChunkData*>> chunks;
    };
    ActiveSnapshot GetActiveSnapshot() const;

    // Snapshot of all kReady chunks (coord + non-owning ptr), for S2 derivation
    // readers that need the full resident set. Same ownership note as above.
    struct ReadySnapshot {
        std::vector<std::pair<ChunkCoord, ChunkData*>> chunks;
    };
    ReadySnapshot GetReadySnapshot() const;

    // The set of region coords (see ChunkToRegionCoord) that still contain at
    // least one registry entry. Used by EventBus::GC to drop region inboxes
    // whose region has no resident chunk (prevents unbounded inbox growth as
    // the player explores). Computed by scanning chunks_ once; cheap relative
    // to a tick.
    std::vector<ChunkCoord> GetResidentRegions() const;

private:
    // Result handoff between the generation worker and the S0 Advance() thread.
    // The worker writes *result_ptr (a fresh TRef<ChunkData>) and sets
    // *done_flag; Advance() reads them. shared_ptr is used for this internal
    // handoff (decoupled from the chunk's own RefCounted lifetime — the chunk
    // only becomes RefCounted-on-the-map once installed). Both are reset when
    // the entry is resolved or unloaded.
    struct GenHandle {
        std::shared_ptr<mi::TRef<ChunkData>> result_ptr;   // worker output slot
        std::shared_ptr<std::atomic<bool>> done_flag;      // worker completion signal
    };

    struct Entry {
        ChunkPresence presence = ChunkPresence::kAbsent;
        ChunkSim sim = ChunkSim::kInactive;
        // Non-owning reference; the owning TRef lives in WorldShellData (handed
        // off via the ChunkInstallSink at install time). Null when not kReady.
        ChunkData* chunk = nullptr;
        mi::TaskGroupRef gen_task_group;             // non-null when presence == kLoading
        GenHandle gen_handle;                        // valid when presence == kLoading
        ShellCategory pending_category = ShellCategory::kEmpty;
        ChunkSim pending_sim = ChunkSim::kInactive;  // sim to apply once generation lands
    };

    // Launch the generation task for an entry (presence kAbsent -> kLoading).
    // `priority` becomes the worldgen task's TaskGraph scheduling priority.
    void LaunchGeneration(Entry& entry, const ChunkCoord& coord,
                          mi::TaskPriority priority);

    std::unordered_map<ChunkCoord, Entry, ChunkCoordHash> chunks_;

    // One generator pointer per category (not owned).
    ChunkGeneratorFn* generators_[static_cast<size_t>(ShellCategory::kMax)] {};

    // Ownership sink — moves generated chunk data into the owning container.
    ChunkInstallSink install_sink_;
    // Ownership remove sink — drops the owning ref from the container on unload.
    ChunkRemoveSink remove_sink_;
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_CHUNK_REGISTRY_H
