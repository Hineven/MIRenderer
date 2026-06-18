/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

// Streaming stress tests for ChunkStreamer + ChunkRegistry + ChunkMeshingContext.
//
// These tests run the REAL streaming stack (real worldgen tasks on the
// TaskGraph, real registry state machine, real meshing scheduler) headless —
// no RHI / window / renderer. They inject a synthetic camera chunk into
// ChunkStreamer::Tick() to drive the envelope, exactly the seam a future
// console command or ZMQ remote control would use.
//
// Fixture mirrors MeshingSchedulerTest (greedy_meshing_test.cpp): MyInfra(true)
// + TaskGraph(4,0) bootstraps worker threads without any GPU dependency.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "meshing/chunk_streamer.h"
#include "meshing/chunk_meshing_context.h"
#include "meshing/types.h"
#include "profiler/profiler.h"
#include "world/chunk_registry.h"
#include "world/chunk_data.h"
#include "world/chunk_coord.h"
#include "world/shell_data.h"
#include "world/world_data.h"
#include "worldgen/simple_terrain_worldgen.h"
#include "worldgen/empty_worldgen.h"
#include "registry/block_registry.h"
#include "core/infra.h"
#include "core/task.h"
#include "infra_impl/infra.h"

using namespace macromc;

namespace {
// Small radius so tests stay fast. R=2 => 5x5 active + 7x7 borderline ring
// = 49 resident chunks. Enough to exercise envelope diff / grace / dirty logic
// without spawning hundreds of worldgen tasks per tick.
constexpr int kTestRadius = 2;
// Expected envelope size for a given radius (active + borderline ring):
//   side = 2*(R+1)+1 = 2R+3, total = side*side.
constexpr size_t EnvelopeSizeFor(int radius) { auto s = 2*radius+3; return size_t(s)*size_t(s); }

// Wait for all in-flight tasks on the TaskGraph to finish (worldgen workers
// drain). TaskGraph has no WaitAll (that's a TaskGroup method), so we poll
// GetPendingTaskCount, which returns to 0 once every task — including
// cancelled ones — has been popped by a worker (see task.cpp:345-356).
// Bounded by a generous timeout so a misbehaving test fails fast instead of
// hanging the suite.
void WaitForPendingTasks(std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (mi::TaskGraph::Get().GetPendingTaskCount() > 0) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Run the streamer for a few ticks, letting the TaskGraph's worldgen workers
// catch up, then drain pending tasks so all in-flight generation is settled.
// Returns the list of chunks that became dirty over those ticks (so a test can
// assert on mesh readiness without coupling to exact worldgen timing).
std::unordered_set<ChunkCoord, ChunkCoordHash> RunTicksAndDrain(
    ChunkStreamer& streamer, ChunkCoord cam_chunk, int ticks) {
    std::unordered_set<ChunkCoord, ChunkCoordHash> all_dirty;
    for (int i = 0; i < ticks; ++i) {
        streamer.Tick(cam_chunk);
        auto d = streamer.ConsumeDirtySet();
        all_dirty.insert(d.begin(), d.end());
    }
    WaitForPendingTasks();
    // One more tick to harvest any generation that finished during the wait.
    streamer.Tick(cam_chunk);
    auto d = streamer.ConsumeDirtySet();
    all_dirty.insert(d.begin(), d.end());
    return all_dirty;
}
} // namespace

// ============================================================================
// Fixture: BlockRegistry + infra + TaskGraph + ChunkRegistry + worldgen wiring
// + ChunkMeshingContext + ChunkStreamer. Mirrors MeshingSchedulerTest.
// ============================================================================
class StreamingStressTest : public ::testing::Test {
protected:
    void SetUp() override {
        GetGlobalBlockRegistry().RegisterBuiltinBlocks();
        mi::TransferInfra(std::make_unique<mi::MyInfra>(true));
        mi::GetInfra().Init();
        mi::TaskGraph::InitializeSingleton(4, 0);

        // WorldData owns chunk data; ChunkRegistry tracks state only. Mirror
        // the app's ownership split (see MacroMCApp::InitWorldSubsystems).
        world_ = mi::Create<WorldData>();
        auto terrain_shell = world_->CreateShell(ShellCategory::kTerrain);
        primary_terrain_shell_ = terrain_shell ? terrain_shell->GetId() : kInvalidShellId;

        registry_ = mi::Create<ChunkRegistry>();
        ctx_ = ChunkMeshingContext::Create(GetGlobalBlockRegistry());

        // Wire worldgens (same adapter pattern as MacroMCApp::InitWorldSubsystems).
        macromc::WorldShellData* null_shell = nullptr;
        terrain_gen_fn_ = [gen = &terrain_worldgen_, null_shell](
                              const ChunkCoord& coord, ChunkData* out_data) {
            gen->GenerateChunk(null_shell, coord, out_data);
        };
        empty_gen_fn_ = [gen = &empty_worldgen_, null_shell](
                            const ChunkCoord& coord, ChunkData* out_data) {
            gen->GenerateChunk(null_shell, coord, out_data);
        };
        registry_->RegisterGenerator(ShellCategory::kTerrain, &terrain_gen_fn_);
        registry_->RegisterGenerator(ShellCategory::kEmpty, &empty_gen_fn_);

        // Ownership sinks: registry hands owning TRef to the terrain shell,
        // keeps the returned non-owning pointer. Remove sink drops the owner.
        registry_->SetChunkInstallSink(
            [this](const ChunkCoord& coord, ShellCategory /*cat*/, mi::TRef<ChunkData> chunk) -> ChunkData* {
                auto shell = world_->GetShell(primary_terrain_shell_);
                if (!shell) return nullptr;
                ChunkData* raw = chunk.Raw();
                shell->SetChunk(coord, std::move(chunk));
                return raw;
            });
        registry_->SetChunkRemoveSink(
            [this](const ChunkCoord& coord) {
                auto shell = world_->GetShell(primary_terrain_shell_);
                if (shell) shell->RemoveChunk(coord);
            });

        ChunkStreamer::Config cfg;
        cfg.active_radius = kTestRadius;
        cfg.unload_grace_seconds = 0.05;  // short, so grace tests can sleep briefly
        streamer_ = ChunkStreamer::Create(cfg);
        streamer_->SetRegistry(registry_.Raw());
        streamer_->SetMeshingContext(ctx_.Raw());

        // Profiler: install as global so worldgen/mesh worker scopes + counters
        // record. Reset between tests via TearDown.
        profiler_ = Profiler::Create();
        Profiler::SetGlobal(profiler_.Raw());
    }

    void TearDown() override {
        // Drain any in-flight worker tasks BEFORE tearing down the subsystems.
        // worldgen/mesh tasks reach into the registry + the global profiler; if
        // they're still running when we destroy those, it's a use-after-free.
        // (Some tests dispatch work without waiting — e.g. JitterAcrossBorder.)
        WaitForPendingTasks();
        Profiler::SetGlobal(nullptr);
        profiler_ = nullptr;
        streamer_ = nullptr;
        ctx_ = nullptr;
        registry_ = nullptr;  // drops non-owning ChunkData* refs
        world_ = nullptr;     // releases the owning TRef<ChunkData> storage
        mi::TaskGraph::DestroySingleton();
        mi::GetInfra().Shutdown();
        mi::DestroyInfra();
    }

    // Helper: fully drain worldgen for the current camera position and return
    // how many registry entries are present (ready + loading).
    size_t DrainAndCountEntries() {
        // worldgen is fast for small radius; a few ticks + drain is enough.
        for (int i = 0; i < 5; ++i) {
            streamer_->Tick(last_cam_);
            (void)streamer_->ConsumeDirtySet();
        }
        WaitForPendingTasks();
        streamer_->Tick(last_cam_);
        (void)streamer_->ConsumeDirtySet();
        return registry_->GetEntryCount();
    }

    macromc::SimpleTerrainWorldgen terrain_worldgen_{12345};
    macromc::EmptyWorldgen empty_worldgen_;
    ChunkGeneratorFn terrain_gen_fn_;
    ChunkGeneratorFn empty_gen_fn_;

    mi::TRef<WorldData> world_;
    ShellId primary_terrain_shell_ = kInvalidShellId;
    mi::TRef<ChunkRegistry> registry_;
    mi::TRef<ChunkMeshingContext> ctx_;
    mi::TRef<ChunkStreamer> streamer_;
    mi::TRef<Profiler> profiler_;
    ChunkCoord last_cam_{0, 0};  // tracks the last cam_chunk fed to Tick()
};

// ============================================================================
// 1. Walking in a line: chunks load near the player and the far ones eventually
//    enter the grace queue (and unload after grace). Envelope size is constant.
// ============================================================================
TEST_F(StreamingStressTest, WalksInLine_ChunksLoadAndUnloadGracefully) {
    // Seed the initial envelope and let worldgen settle.
    last_cam_ = {0, 0};
    RunTicksAndDrain(*streamer_, last_cam_, 3);

    // The envelope should be the full chebyshev ball around the camera.
    EXPECT_EQ(streamer_->GetEnvelopeSize(), EnvelopeSizeFor(kTestRadius));

    // Walk +X for a while. After each step the far -X chunks should leave the
    // envelope and enter the grace queue; the near +X chunks should load.
    size_t pending_seen = 0;
    for (int step = 1; step <= 20; ++step) {
        last_cam_ = {step, 0};
        streamer_->Tick(last_cam_);
        (void)streamer_->ConsumeDirtySet();

        // Envelope size stays constant as we walk — this is the core invariant.
        EXPECT_EQ(streamer_->GetEnvelopeSize(), EnvelopeSizeFor(kTestRadius));

        // Chunks far behind us (-X) that left the envelope should accumulate in
        // the grace queue (not yet unloaded, since the loop runs far faster than
        // the grace window). This proves exits are tracked.
        pending_seen = std::max(pending_seen, streamer_->GetPendingUnloadCount());
    }
    // We walked far enough that exits must have started queueing.
    EXPECT_GT(pending_seen, 0u);

    // Registry entries are bounded: envelope + grace backlog. Walking 20 steps
    // with a 7-wide (R+1=3) front advances ~7 new chunks per step, but grace
    // holds the ones behind. The total never grows without bound; assert it's
    // well under the naive worst case (20 steps * front width).
    size_t entries_during_walk = registry_->GetEntryCount();
    EXPECT_LE(entries_during_walk, EnvelopeSizeFor(kTestRadius) * 5);

    // Now let the grace window expire and tick again. Chunks that left the
    // envelope get unloaded, so the registry shrinks back toward just the
    // current envelope. (grace = 0.05s in the fixture config.)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    streamer_->Tick(last_cam_);
    (void)streamer_->ConsumeDirtySet();

    // After grace flush, entries are bounded by the envelope again (unload
    // tears down each expired chunk). Allow a little slack for chunks that
    // finished worldgen right at the boundary.
    size_t entries_after_grace = registry_->GetEntryCount();
    EXPECT_LE(entries_after_grace, EnvelopeSizeFor(kTestRadius) + 5)
        << "grace-flushed registry should be near the envelope size, got "
        << entries_after_grace;
    // And critically, the pending-unload queue drained significantly.
    EXPECT_LT(streamer_->GetPendingUnloadCount(), pending_seen);
}

// ============================================================================
// 2. Jitter across a chunk border: a chunk near the seam must not thrash
//    load/unload. The grace queue absorbs the oscillation.
// ============================================================================
TEST_F(StreamingStressTest, JitterAcrossBorder_NoThrashLoad) {
    last_cam_ = {0, 0};
    RunTicksAndDrain(*streamer_, last_cam_, 3);
    size_t entries_before = registry_->GetEntryCount();

    // Bounce between (0,0) and (1,0) 20 times. With grace=0.05s this is far
    // faster than the grace window, so no chunk should actually unload.
    for (int i = 0; i < 20; ++i) {
        last_cam_ = {(i % 2), 0};
        streamer_->Tick(last_cam_);
        (void)streamer_->ConsumeDirtySet();
    }
    // The union of the two envelopes is only slightly larger than one (a column
    // shifts by one), so entries must not explode. Grace prevents unload here.
    size_t entries_after = registry_->GetEntryCount();
    EXPECT_LE(entries_after, entries_before + EnvelopeSizeFor(kTestRadius));
    // And critically, no chunk should have been unloaded mid-jitter (grace not
    // expired): the pending-unload set may be non-empty, but the registry did
    // not shrink below the single-envelope baseline.
    EXPECT_GE(entries_after, EnvelopeSizeFor(kTestRadius));
}

// ============================================================================
// 3. Regression: a chunk that leaves the envelope while still kLoading must be
//    tracked for unload (the envelope set — not a registry ready-snapshot — is
//    the source of truth). Previously a kLoading chunk that exited would be
//    missed and become an orphan.
// ============================================================================
TEST_F(StreamingStressTest, OrphanChunk_KLoadingExitIsTracked) {
    // Use a fresh streamer with zero grace so we can deterministically force an
    // unload right after exit, regardless of wall-clock timing.
    ChunkStreamer::Config cfg;
    cfg.active_radius = kTestRadius;
    cfg.unload_grace_seconds = 0.0;  // unload immediately on FlushGraceQueue
    auto fast_streamer = ChunkStreamer::Create(cfg);
    fast_streamer->SetRegistry(registry_.Raw());
    fast_streamer->SetMeshingContext(ctx_.Raw());

    // Step 1: stand at (0,0) and tick ONCE. This issues RequestLoad for the
    // whole envelope; most chunks are still kLoading (worldgen hasn't run yet).
    fast_streamer->Tick({0, 0});
    (void)fast_streamer->ConsumeDirtySet();
    // Confirm we have loading entries (worldgen not yet drained).
    size_t entries_after_one_tick = registry_->GetEntryCount();
    ASSERT_GT(entries_after_one_tick, 0u);

    // Step 2: jump far away (outside the old envelope entirely). Every chunk
    // from the (0,0) envelope is now an exit — including kLoading ones. With
    // zero grace, the next Tick must flush (unload) all of them.
    ChunkCoord far_away{1000, 1000};
    fast_streamer->Tick(far_away);
    (void)fast_streamer->ConsumeDirtySet();

    // The (0,0) envelope chunks must all be gone from the registry now: zero
    // grace means FlushGraceQueue unloaded every exit, kLoading or not. This is
    // the orphan-chunk regression guard.
    for (int dx = -(kTestRadius+1); dx <= (kTestRadius+1); ++dx) {
        for (int dz = -(kTestRadius+1); dz <= (kTestRadius+1); ++dz) {
            EXPECT_FALSE(registry_->Has({dx, dz}))
                << "chunk {" << dx << "," << dz << "} should have been unloaded";
        }
    }
    // But the far-away envelope loaded instead.
    EXPECT_TRUE(registry_->Has(far_away));
}

// ============================================================================
// 4. When a chunk becomes ready, its already-resident neighbours are marked
//    dirty (so S2 can re-mesh them to cull the now-shared boundary faces).
// ============================================================================
TEST_F(StreamingStressTest, NewlyReadyMarksNeighboursDirty) {
    last_cam_ = {0, 0};
    // Tick once to issue loads; drain worldgen so everything becomes ready.
    auto dirty = RunTicksAndDrain(*streamer_, last_cam_, 1);

    // The center chunk (0,0) became ready this pass -> it's dirty.
    EXPECT_TRUE(dirty.count(ChunkCoord{0, 0}) > 0);
    // Its neighbours also became ready and should be dirty too. Pick an
    // in-envelope neighbour and confirm.
    ChunkCoord neighbour{1, 0};
    ASSERT_TRUE(streamer_->IsInEnvelope(neighbour));
    EXPECT_TRUE(dirty.count(neighbour) > 0)
        << "resident neighbour of a newly-ready chunk should be marked dirty";
}

// ============================================================================
// 5. ConsumeDirtySet drains: after consuming, the dirty set is empty until the
//    next Advance produces new ready chunks.
// ============================================================================
TEST_F(StreamingStressTest, ConsumeDirtySetDrainsUntilNextHarvest) {
    last_cam_ = {0, 0};
    auto dirty = RunTicksAndDrain(*streamer_, last_cam_, 1);
    ASSERT_FALSE(dirty.empty());
    // After consuming, the streamer's dirty set is empty.
    EXPECT_EQ(streamer_->GetDirtySetSize(), 0u);
    // A tick with no new completions produces no dirty entries.
    streamer_->Tick(last_cam_);
    EXPECT_EQ(streamer_->GetDirtySetSize(), 0u);
    (void)streamer_->ConsumeDirtySet();
}

// ============================================================================
// 6. S2→S3 pipeline: dirty chunks get meshed (S2), and the results can be
//    polled via GetResult (S3). This mirrors the app's StageS2/StageS3 logic
//    but drives it through the public meshing-context API, verifying that a
//    chunk requested in S2 produces a non-empty mesh result pollable in S3
//    once the worker finishes. (The in-flight/pending containers themselves
//    are app-internal; this test exercises the GetResult contract they rely on.)
// ============================================================================
TEST_F(StreamingStressTest, S3PollsCompletedMeshesFromS2) {
    last_cam_ = {0, 0};
    // S0: produce some ready + dirty chunks.
    auto dirty = RunTicksAndDrain(*streamer_, last_cam_, 2);
    ASSERT_FALSE(dirty.empty());

    // S2 (replica): register + request mesh for each dirty chunk, track coords.
    // Find() returns a non-owning presence check; the owning TRef lease for the
    // meshing context comes from WorldShellData (the sole owner).
    std::unordered_set<ChunkCoord, ChunkCoordHash> in_flight;
    auto shell = world_->GetShell(primary_terrain_shell_);
    for (const auto& coord : dirty) {
        if (!registry_->Find(coord)) continue;  // presence check
        auto chunk = shell ? shell->GetChunk(coord) : nullptr;
        if (!chunk) continue;
        ctx_->RegisterChunk(coord, chunk);
        ctx_->RequestMesh(coord, last_cam_);
        in_flight.insert(coord);
    }
    ASSERT_FALSE(in_flight.empty());

    // S3 (replica) attempt 1: immediately after dispatch, most meshes are still
    // running on workers — GetResult should return null for them.
    std::unordered_map<ChunkCoord, std::shared_ptr<VoxelMeshJobResult>, ChunkCoordHash>
        pending;
    for (auto it = in_flight.begin(); it != in_flight.end();) {
        auto r = ctx_->GetResult(*it);
        if (r) { pending[*it] = std::move(r); it = in_flight.erase(it); }
        else { ++it; }
    }

    // Drain the workers, then S3 again: everything should complete now.
    WaitForPendingTasks();
    for (auto it = in_flight.begin(); it != in_flight.end();) {
        auto r = ctx_->GetResult(*it);
        if (r) { pending[*it] = std::move(r); it = in_flight.erase(it); }
        else { ++it; }
    }

    // Every dispatched chunk must have produced a result.
    EXPECT_TRUE(in_flight.empty());
    EXPECT_FALSE(pending.empty());

    // At least one pending result must have real geometry (terrain chunks
    // generate surface faces). Sum triangles across all subchunks of one result.
    bool any_non_empty = false;
    for (const auto& [coord, result] : pending) {
        for (const auto& sub : result->subchunk_meshes) {
            if (!sub.IsEmpty()) { any_non_empty = true; break; }
        }
        if (any_non_empty) break;
    }
    EXPECT_TRUE(any_non_empty) << "expected at least one non-empty mesh result";
}

// ============================================================================
// 7. Performance profile: walk a long line under a realistic envelope, then
//    report worldgen throughput, total chunks generated, and mean tick time.
//    This is a measurement test (no hard pass/fail threshold — it prints the
//    numbers); it only asserts that work actually happened (chunks > 0).
//
//    Uses a dedicated ChunkStreamer with active_radius=8 (19x19=361 envelope,
//    matching CHUNK_REGISTRY_DESIGN §5.1) so the numbers reflect a realistic
//    render distance rather than the tiny R=2 the correctness tests use.
// ============================================================================
TEST_F(StreamingStressTest, PerformanceProfile_WorldgenThroughputAndTickTime) {
    constexpr int kRealisticRadius = 8;
    constexpr int kSteps = 40;  // walk 40 chunks along +X

    // Standalone streamer with the realistic radius (the fixture's streamer_
    // uses kTestRadius=2 for the correctness tests; reuse its registry/ctx).
    ChunkStreamer::Config cfg;
    cfg.active_radius = kRealisticRadius;
    cfg.unload_grace_seconds = 0.05;
    auto streamer = ChunkStreamer::Create(cfg);
    streamer->SetRegistry(registry_.Raw());
    streamer->SetMeshingContext(ctx_.Raw());

    profiler_->Reset();  // start clean

    // Each "tick": advance the camera by one chunk, run the streamer tick under
    // a profiler scope (so MergeFrame aggregates it), and let worldgen settle a
    // little. We don't fully drain every tick (that would hide streaming lag);
    // instead we drain once at the end so the timer captures real per-chunk cost.
    auto wall_start = std::chrono::high_resolution_clock::now();
    double tick_ns_total = 0;
    int tick_count = 0;
    ChunkCoord cam{0, 0};
    for (int step = 0; step <= kSteps; ++step) {
        cam = {step, 0};
        auto tick_begin = std::chrono::high_resolution_clock::now();
        {
            auto scope = profiler_->Scope("tick.streaming_step");
            streamer->Tick(cam);
            (void)streamer->ConsumeDirtySet();
        }
        auto tick_end = std::chrono::high_resolution_clock::now();
        tick_ns_total += std::chrono::duration_cast<std::chrono::nanoseconds>(
                             tick_end - tick_begin).count();
        ++tick_count;
        profiler_->MergeFrame();  // fold this step's worker data in
    }
    // Final drain so all in-flight worldgen completes and gets counted.
    WaitForPendingTasks();
    streamer->Tick(cam);
    (void)streamer->ConsumeDirtySet();
    profiler_->MergeFrame();
    auto wall_end = std::chrono::high_resolution_clock::now();

    // --- Gather metrics ---
    auto snaps = profiler_->GetSnapshot();
    const MetricSnapshot* wg_single = nullptr;
    const MetricSnapshot* wg_ready = nullptr;
    const MetricSnapshot* tick_step = nullptr;
    uint64_t wg_total_ns = 0, wg_calls = 0, chunks_ready = 0;
    for (const auto& m : snaps) {
        if (m.name == "worldgen.single") { wg_single = &m; wg_total_ns = m.total_ns; wg_calls = m.call_count; }
        if (m.name == "worldgen_chunks_ready") { wg_ready = &m; chunks_ready = m.value; }
        if (m.name == "tick.streaming_step") tick_step = &m;
    }

    double wall_s = std::chrono::duration<double>(wall_end - wall_start).count();
    double mean_tick_us = (tick_count > 0) ? (tick_ns_total / 1000.0 / tick_count) : 0.0;
    double wg_total_s = wg_total_ns / 1e9;
    double wg_mean_us = (wg_calls > 0) ? (wg_total_ns / 1000.0 / wg_calls) : 0.0;
    // Throughput: chunks generated per wall-clock second of worldgen work.
    double chunks_per_sec = (wg_total_s > 0) ? (static_cast<double>(wg_calls) / wg_total_s) : 0.0;

    // --- Report (always printed; no threshold so it's stable across machines) ---
    MI_LOG(mi::MIInfraLogType::kInfo,
           "========================================");
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf] streaming profile (radius={}, steps={}):", kRealisticRadius, kSteps);
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf]   chunks generated (worldgen.single calls): {}", wg_calls);
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf]   chunks became ready (counter):           {}", chunks_ready);
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf]   worldgen mean per-chunk:   {:.1f} us", wg_mean_us);
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf]   worldgen throughput:       {:.1f} chunks/s", chunks_per_sec);
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf]   worldgen total time:       {:.2f} ms", wg_total_s * 1000.0);
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf]   mean streaming-step time:  {:.1f} us (over {} steps)",
           mean_tick_us, tick_count);
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf]   wall-clock total:          {:.2f} ms", wall_s * 1000.0);
    MI_LOG(mi::MIInfraLogType::kInfo,
           "[perf]   registry entries now:      {}", registry_->GetEntryCount());
    MI_LOG(mi::MIInfraLogType::kInfo,
           "========================================");

    // Only sanity-check that work happened; numbers are informational.
    EXPECT_GT(wg_calls, 0u);
    EXPECT_GT(chunks_ready, 0u);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
