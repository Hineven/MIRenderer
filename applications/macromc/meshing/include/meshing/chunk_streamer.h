/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_MESHING_CHUNK_STREAMER_H
#define MACROMC_MESHING_CHUNK_STREAMER_H

#include <chrono>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "meshing/common.h"
#include "world/chunk_data.h"
#include "world/chunk_registry.h"
#include "world/shell_data.h"   // ChunkCoordHash
#include "world/types.h"
#include "core/refcounted.h"
#include "core/task.h"

MACROMC_MESHING_NAMESPACE_BEGIN

// =============================================================================
// ChunkStreamer — the streaming driver extracted out of MacroMCApp.
//
// Owns the S0-stage streaming logic per TICK_PACING §3.1 + CHUNK_REGISTRY_DESIGN
// §5: given a camera/player chunk each tick, it keeps the ChunkRegistry's
// resident set in sync with a cubic-column envelope (active core + borderline
// neighbour ring), drives graceful (grace-timed) unload of chunks that left the
// envelope, harvests finished async worldgen, and marks the dirty set for S2.
//
// Design notes:
//   - Decoupled from the renderer. The caller computes cam_chunk from whatever
//     "player position" source it has (a fly camera, an entity, a test harness,
//     or a console command) and passes it to Tick(). This is the seam that lets
//     tests run the real streaming stack headless, and that lets a future
//     CommandRegistry command drive the world from a script.
//   - S2 mesh dispatch stays in the app: ChunkStreamer only *produces* the dirty
//     set (ConsumeDirtySet); the app decides how/when to RequestMesh. This
//     preserves the S0/S2 stage separation (registry stable before meshing).
//   - EventBus GC is NOT done here — it's an EventBus concern, not a streaming
//     concern; the app runs it in S0 after Tick().
//
// Threading contract:
//   - All methods are called ONLY from the S0 single thread (render/main).
//     The registry and meshing context are mutated through their own
//     single-threaded APIs; the dirty set / envelope / pending_unloads are
//     owned here and touched only by this thread.
// =============================================================================

// Forward declaration of ChunkMeshingContext avoids a heavy include here.
class ChunkMeshingContext;

class ChunkStreamer : public mi::RefCounted<> {
public:
    struct Config {
        // Chebyshev radius of the active core in the XZ plane. Chunks within
        // this radius are kActive (tick in S1).
        int active_radius = 8;

        // Grace period before a chunk that left the envelope is actually
        // unloaded. Prevents load/unload thrash when the player jitters across
        // a chunk border.
        double unload_grace_seconds = 10.0;

        // Shell category assigned to every requested load. Single-shell world
        // for now; multi-shell selection comes with the fracture feature.
        ShellCategory shell_category = ShellCategory::kTerrain;
    };

    static mi::TRef<ChunkStreamer> Create(Config config = {});

    // Wire the subsystems. Non-owning pointers; the caller must keep them alive
    // for the streamer's lifetime. The meshing context is optional; when null,
    // dirty-harvest still records neighbours as dirty (useful for tests that
    // only exercise registry behaviour), but unload won't UnregisterChunk.
    void SetRegistry(ChunkRegistry* registry);
    void SetMeshingContext(ChunkMeshingContext* meshing);

    // One streaming tick. cam_chunk is the player/camera's chunk (XZ column),
    // computed by the caller. Internally:
    //   1. UpdateEnvelope(cam_chunk) — only if cam_chunk changed since last tick.
    //      Issues RequestLoad / SetSimState for new entries, queues exits into
    //      pending_unloads_ (grace).
    //   2. FlushGraceQueue() — unloads chunks whose grace expired.
    //   3. registry_->Advance() — harvests finished worldgen; newly-ready chunks
    //      + their resident neighbours are added to the dirty set.
    void Tick(ChunkCoord cam_chunk);

    // Drain and return the dirty set. Called by the app's S2 stage to decide
    // which chunks to RegisterChunk + RequestMesh. The set is cleared on read.
    std::unordered_set<ChunkCoord, ChunkCoordHash> ConsumeDirtySet();

    // --- Query surface (for tests + console introspection) ---
    size_t GetPendingUnloadCount() const { return pending_unloads_.size(); }
    size_t GetEnvelopeSize() const { return envelope_.size(); }
    size_t GetDirtySetSize() const { return dirty_set_.size(); }
    bool IsInEnvelope(ChunkCoord c) const { return envelope_.count(c) > 0; }
    bool IsPendingUnload(ChunkCoord c) const { return pending_unloads_.count(c) > 0; }
    bool IsDirty(ChunkCoord c) const { return dirty_set_.count(c) > 0; }

private:
    explicit ChunkStreamer(Config config);

    // Reconcile the envelope against cam_chunk (no-op if cam_chunk unchanged).
    void UpdateEnvelope(ChunkCoord cam_chunk);
    // Unload grace-expired chunks (cancels their meshing + drops the entry).
    void FlushGraceQueue();

    Config config_;
    ChunkRegistry* registry_ = nullptr;
    ChunkMeshingContext* meshing_ = nullptr;

    // Chunks currently inside the envelope (active + borderline). Authoritative
    // record of "what we asked the registry to load". Diffed tick-to-tick.
    std::unordered_set<ChunkCoord, ChunkCoordHash> envelope_;
    // Last cam_chunk the envelope was built around. nullopt = not built yet.
    std::optional<ChunkCoord> envelope_cam_chunk_;
    // Chunks that left the envelope, awaiting grace-expired unload.
    std::unordered_map<ChunkCoord, std::chrono::steady_clock::time_point,
                       ChunkCoordHash> pending_unloads_;
    // Chunks needing (re-)mesh, drained by the app's S2.
    std::unordered_set<ChunkCoord, ChunkCoordHash> dirty_set_;
};

MACROMC_MESHING_NAMESPACE_END

#endif // MACROMC_MESHING_CHUNK_STREAMER_H
