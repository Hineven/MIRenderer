/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#pragma once

#include <atomic>
#include <memory>
#include <future>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "infra_impl/infra.h"
#include "renderer/mi_renderer.h"
#include "rdg/rdg.h"
#include "core/task.h"

#include "world/chunk_registry.h"
#include "world/event_bus.h"
#include "world/world_data.h"
#include "meshing/chunk_meshing_context.h"
#include "meshing/chunk_streamer.h"
#include "meshing/types.h"
#include "registry/block_registry.h"
#include "profiler/profiler.h"
#include "worldgen/simple_terrain_worldgen.h"
#include "worldgen/empty_worldgen.h"
#include "render_command.h"
#include "render_thread_context.h"
#include "gigavoxel_shell_handle.h"

struct GLFWwindow;

MACROMC_NAMESPACE_BEGIN

// Start configuration for the MacroMC main loop.
struct MacroMCStartConfig {
    std::string window_name = "MacroMC";
    uint32_t window_width = 1280;
    uint32_t window_height = 720;
};

// =============================================================================
// MacroMCApp — the tick-pacing-driven voxel application host.
//
// Mirrors the ViewerApp lifecycle (Initialize -> Run -> Destroy) but the Run()
// loop is split into the TICK_PACING stages (S0 Registry, S1 Simulation,
// S2 Derivation, S3 Upload, boundary, S4 LoosePhysics) driven by a fixed-timestep
// accumulator. This is the spine that wires ChunkRegistry / EventBus /
// ChunkMeshingContext to the renderer.
//
// Stage methods are placeholders for now (S0/S2 call into the registry/mesher;
// S1/S3/S4 are stubs) — the goal of this skeleton is to establish the tick
// structure and verify the pacing contract end-to-end.
// =============================================================================
class MacroMCApp {
public:
    MacroMCApp() = default;
    ~MacroMCApp() = default;

    MacroMCApp(const MacroMCApp&) = delete;
    MacroMCApp& operator=(const MacroMCApp&) = delete;

    // Lifecycle (mirrors ViewerApp).
    void Initialize(std::unique_ptr<mi::MIInfraInterface> infra, const MacroMCStartConfig& cfg);
    void Destroy();
    void Run();

    // Read-only accessors for console command registration (see
    // macromc_app_commands.cpp). The profiler is owned by the app; commands
    // capture the raw pointer, safe because commands live no longer than the app.
    Profiler* GetProfiler() const { return profiler_.Raw(); }

private:
    // --- Window / RHI setup helpers (extracted from ViewerApp) ---
    void StartWindow(const MacroMCStartConfig& cfg);
    void InitRHI();
    void InitTaskSystem();
    void InitRenderer();
    void InitWorldSubsystems();

    // --- Tick stages (TICK_PACING contract). ---
    // TickGameplay runs the simulation stages on the gameplay thread (or main,
    // in bypass mode): S0→S1→S2→boundary→S4. S3 (render upload) is NOT here —
    // it's driven by the render thread via RenderThreadContext.
    void TickGameplay();

    // S0: chunk load/unload, registry Advance, active-set update. Single-thread.
    void StageS0_Registry();
    // S1: gameplay tick (checkerboard). Stub.
    void StageS1_Simulation();
    // S2: mesh rebuild + collision derivation. Parallel, drains dirty set.
    void StageS2_Derivation();
    // boundary: gameplay→render command dispatch. Pushes UpdateCamera (and
    // future UploadChunkMesh) into the render command queue. Was a stage
    // separator in single-thread; now the real cross-thread handoff point.
    void StageBoundary();
    // S4: loose physics, crosses into N+1. Stub.
    void StageS4_LoosePhysics();

    // --- S3 (render thread side) ---
    // Polls in-flight meshes, moves completed ones to pending_uploads_. Runs
    // on the render thread (called from TickRender via the render callback).
    void StageS3_Upload();

    // --- Rendering (consume S3 output). Stub-level for now. ---
    void RenderFrame();

    // --- Thread entry points ---
    void RunSingleThreaded();   // bypass: glfw + TickGameplay + render TickRender, serial
    void RunMultiThreaded();    // main only does glfw+input; gameplay+render are separate threads
    void GameplayThreadLoop();  // the gameplay thread body

    // Build a UpdateCameraCmd from the current view_->camera_ (gameplay side).
    UpdateCameraCmd MakeCameraCommand() const;

    // --- Members: window / RHI / renderer (mirrors ViewerApp) ---
    GLFWwindow* window_ = nullptr;
    mi::TRef<mi::RDGResourcePool> pool_;
    mi::TRef<mi::DeviceBindlessResourceAllocator> resource_allocator_;
    std::unique_ptr<mi::Scene> scene_;
    std::unique_ptr<mi::RendererView> view_;
    // NOTE: previous_frame_future_ / previous_frame_sync_point_ moved to
    // RenderThreadContext (frame-overlap state is owned by the render side).

    // --- Thread model (see THREAD_MODEL.md) ---
    // The render-thread side: owns the command queue, frame-overlap sync, and
    // (when not bypassing) the render thread itself.
    std::unique_ptr<RenderThreadContext> render_ctx_;
    // The gameplay thread (non-bypass). Runs the fixed-TPS simulation loop.
    std::thread gameplay_thread_;
    // Stop flag shared by main/gameplay/render for orderly shutdown.
    std::atomic<bool> stop_requested_{false};
    // From MACROMC_BYPASS_RENDER_THREAD: true = single-threaded (debug),
    // false = real three-thread split. Set once at Initialize.
    bool bypass_render_thread_ = true;

    // --- MacroMC world subsystems ---
    // WorldData is the sole owner of ChunkData (each shell holds owning
    // TRef<ChunkData>). ChunkRegistry only tracks chunk state (presence/sim)
    // and holds non-owning ChunkData* pointers; the install/remove sinks below
    // bridge the two. The primary terrain shell is where the streaming envelope
    // deposits generated chunks.
    mi::TRef<WorldData> world_data_;
    ShellId primary_terrain_shell_ = kInvalidShellId;
    mi::TRef<ChunkRegistry> chunk_registry_;
    mi::TRef<EventBus> event_bus_;
    mi::TRef<ChunkMeshingContext> meshing_context_;
    // The streaming driver: envelope maintenance, graceful unload, worldgen
    // harvest, dirty-set production. Extracted out so it can be driven headless
    // (tests) or from a console command without depending on the renderer.
    mi::TRef<ChunkStreamer> streamer_;
    // CPU profiler (timers + counters + gauges). Installed as the global
    // profiler at init so worker threads (worldgen/mesh) can reach it via
    // Profiler::GetGlobal() / MI_PROF_* macros without holding a ref.
    mi::TRef<Profiler> profiler_;

    // --- S2→S3 mesh result pipeline ---
    // S2 dispatches mesh tasks for dirty chunks and records their coords here;
    // S3 polls GetResult on them. Completed meshes are flattened (VoxelVertex →
    // GigaVoxelVertex) and uploaded into the GigaVoxel asset directly through
    // the render-side shell registry (S3 runs on the render thread, so it
    // bypasses the SPSC command queue — that queue is for game→render only).
    std::unordered_set<ChunkCoord, ChunkCoordHash> meshes_in_flight_;
    std::unordered_map<ChunkCoord, std::shared_ptr<VoxelMeshJobResult>,
                       ChunkCoordHash> pending_uploads_;

    // The block registry is a global singleton; we hold a convenience pointer.
    BlockRegistry* block_registry_ = nullptr;

    // --- GigaVoxel shell handles (game-thread side, per shell) ---
    // One handle per shell that has a render projection. The handle is pure
    // logical state — it pushes RenderCommands; the render-side
    // GigaVoxelShellRegistry (inside RenderThreadContext) owns the real assets.
    std::unordered_map<ShellId, GigaVoxelShellHandle> shell_handles_;

    // --- Worldgen wiring ---
    // The registry holds non-owning ChunkGeneratorFn* pointers keyed by shell
    // category, so the fn objects themselves must live at stable addresses for
    // the app's lifetime. They are stored here as direct members (not in a
    // container, which could reallocate and move them). Each fn adapts the
    // 3-arg Worldgen::GenerateChunk(shell, coord, data) down to the 2-arg
    // ChunkGeneratorFn(coord, data) signature the registry expects — the shell
    // arg is passed as nullptr (current worldgens ignore it; it exists for
    // future structure-placement generators that need shell context).
    macromc::SimpleTerrainWorldgen terrain_worldgen_{12345};  // fixed seed for now
    macromc::EmptyWorldgen empty_worldgen_;
    ChunkGeneratorFn terrain_gen_fn_;
    ChunkGeneratorFn empty_gen_fn_;

    // Fixed-timestep accumulator (standard game loop).
    double tick_accumulator_ = 0.0;
    static constexpr double kFixedTickDt = 1.0 / 60.0;  // 60 Hz simulation
};

// Register MacroMC console commands (mc.profiler.dump / .reset) into the
// global mi::CommandRegistry. Call once after the profiler is created.
void RegisterMacroMCCommands(MacroMCApp& app);

// Entry point used by main().
void RunMacroMC(std::unique_ptr<mi::MIInfraInterface> infra, const MacroMCStartConfig& cfg);

MACROMC_NAMESPACE_END
