/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "macromc_app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/thr.h"
#include "core/task.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_renderer_view.h"
#include "rdg/rdg_pool.h"
#include "rdg/rdg_shader.h"
#include "rhi/rhi.h"
#include "rhi/vk/vk_export.h"

#include "world/chunk_coord.h"
#include "world/shell_data.h"

#include <renderer/mi_giga_voxel.h>
#include <renderer/mi_texture.h>
#include <core/pixel_format.h>

#include <array>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>

MACROMC_NAMESPACE_BEGIN

// The app layer lives on top of the `mi` renderer/RHI/RDG stack; pull those
// names in directly (mirrors ViewerApp, which compiles inside MI_NAMESPACE).
using namespace mi;

namespace {

// ---- GigaVoxel bridge helpers (mirrors viewer_gigavoxel.cpp) ---------------
// These convert macromc meshing output (VoxelVertex / VoxelMeshJobResult) into
// the renderer-layer GigaVoxelVertex + flattened index list, and build the
// placeholder block atlas. Kept here (duplicated from viewer) rather than
// shared across targets to avoid a cross-application dependency.

// Convert one macromc VoxelVertex into the renderer-layer GigaVoxelVertex.
mi::GigaVoxelVertex ToGigaVoxelVertex(const MACROMC_MESHING_NAMESPACE::VoxelVertex & v) {
    mi::GigaVoxelVertex out;
    out.position = v.position;
    out.normal = v.normal;
    out.uv_base = v.uv_base;
    out.uv_scale = v.uv_scale;
    out.texture_index = v.texture_index;
    out._pad = 0;
    return out;
}

// Flatten a chunk's VoxelMeshJobResult (256 subchunk meshes) into a single
// vertex/index pair with indices re-based into the merged vertex range.
struct FlattenedChunk {
    std::vector<mi::GigaVoxelVertex> vertices;
    std::vector<uint32_t> indices;
};
FlattenedChunk FlattenChunkMesh(const std::shared_ptr<MACROMC_MESHING_NAMESPACE::VoxelMeshJobResult> & result) {
    FlattenedChunk out;
    if (!result) return out;
    for (const auto & sub : result->subchunk_meshes) {
        if (sub.IsEmpty()) continue;
        uint32_t base = static_cast<uint32_t>(out.vertices.size());
        for (const auto & v : sub.vertices) out.vertices.push_back(ToGigaVoxelVertex(v));
        for (auto idx : sub.indices) out.indices.push_back(idx + base);
    }
    return out;
}

// Placeholder 4096^2 atlas: one solid color per builtin block tile. Enough to
// exercise the atlas bindless path; real atlas baking is out of scope.
TRef<mi::Texture> BuildPlaceholderAtlas() {
    struct Color { uint8_t r, g, b, a; };
    const std::array<Color, 8> tile_colors = {{
        {128, 128, 128, 255}, // 0: stone
        {120,  80,  50, 255}, // 1: dirt (also grass bottom)
        { 90, 160,  70, 255}, // 2: legacy grass (unused)
        { 40,  40,  40, 255}, // 3: bedrock
        {220, 200, 140, 255}, // 4: sand
        { 60, 110, 200, 255}, // 5: water
        {110, 180,  80, 255}, // 6: grass top
        {130, 100,  60, 255}, // 7: grass side
    }};
    auto atlas = mi::Texture::Create(mi::PixelFormatType::kR8G8B8A8_UNORM,
        MACROMC_MESHING_NAMESPACE::kAtlasTilesPerRow * MACROMC_MESHING_NAMESPACE::kAtlasTileTexelSize,
        MACROMC_MESHING_NAMESPACE::kAtlasTilesPerCol * MACROMC_MESHING_NAMESPACE::kAtlasTileTexelSize, 1, 1);
    atlas->SetName("GigaVoxelAtlasPlaceholder");
    auto & data = const_cast<std::vector<uint8_t> &>(atlas->GetBinary());
    const uint32_t aw = atlas->GetWidth();
    const uint32_t tile = MACROMC_MESHING_NAMESPACE::kAtlasTileTexelSize;
    const uint32_t tpr = MACROMC_MESHING_NAMESPACE::kAtlasTilesPerRow;
    Color fill {128, 128, 128, 255};
    for (uint32_t y = 0; y < atlas->GetHeight(); ++y)
        for (uint32_t x = 0; x < aw; ++x) {
            size_t o = (y * aw + x) * 4;
            data[o] = fill.r; data[o+1] = fill.g; data[o+2] = fill.b; data[o+3] = fill.a;
        }
    for (uint32_t t = 0; t < tile_colors.size(); ++t) {
        uint32_t tx = t % tpr, ty = t / tpr;
        const Color & c = tile_colors[t];
        for (uint32_t dy = 0; dy < tile; ++dy)
            for (uint32_t dx = 0; dx < tile; ++dx) {
                uint32_t x = tx * tile + dx, y = ty * tile + dy;
                size_t o = (y * aw + x) * 4;
                data[o] = c.r; data[o+1] = c.g; data[o+2] = c.b; data[o+3] = c.a;
            }
    }
    atlas->UpdateOnDevice();
    atlas->ConvertToBindless();
    return atlas;
}

} // namespace

// =============================================================================
// Lifecycle entry point.
// =============================================================================
void RunMacroMC(std::unique_ptr<MIInfraInterface> infra, const MacroMCStartConfig& cfg) {
    MacroMCApp app;
    app.Initialize(std::move(infra), cfg);
    app.Run();
    RHI::Get().WaitForIdle();
    app.Destroy();
}

// =============================================================================
// Helpers — RHI / window / task system / renderer init.
// These are a trimmed mirror of ViewerApp::Initialize, with ImGui/ZMQ/NGX/DLSS
// stripped out. The macromc app is about the tick structure, not the renderer.
// =============================================================================
void MacroMCApp::StartWindow(const MacroMCStartConfig& cfg) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    window_ = glfwCreateWindow(
        static_cast<int>(cfg.window_width),
        static_cast<int>(cfg.window_height),
        cfg.window_name.c_str(), nullptr, nullptr);
    if (!window_) {
        throw std::runtime_error("MacroMC: failed to create GLFW window");
    }
}

void MacroMCApp::InitRHI() {
    std::vector<const char*> inst_extensions;
    uint32_t ext_count = 0;
    auto glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    for (uint32_t i = 0; i < ext_count; ++i) inst_extensions.push_back(glfw_exts[i]);

    VulkanRHICreateInfo info {};
    info.extra_instance_extension_count = static_cast<uint32_t>(inst_extensions.size());
    info.extra_instance_extensions = inst_extensions.data();
    info.extra_device_extension_count = 0;
    info.extra_device_extensions = nullptr;
    RHI::InitializeSingleton(RHIType::kVulkan, &info);
    RHI::Get().ResetPipelineCache(4 * 1024 * 1024);
}

void MacroMCApp::InitTaskSystem() {
    auto limits = GetInfra().GetResourceLimits();
    int hpt = std::max(limits.max_high_performance_thread_count - 2, 0);
    if (hpt < 1) hpt = 1;
    TaskGraph::InitializeSingleton(0, hpt);
}

void MacroMCApp::InitRenderer() {
    pool_ = RDGResourcePool::Create();
    resource_allocator_ = Create<DeviceBindlessResourceAllocator>();
    scene_ = std::make_unique<Scene>();
    view_ = std::make_unique<RendererView>();
    view_->scene_ = scene_.get();
    Renderer::Get().Init(resource_allocator_.Raw(), pool_.Raw());
}

void MacroMCApp::InitWorldSubsystems() {
    // Block registry (global singleton).
    block_registry_ = &GetGlobalBlockRegistry();
    block_registry_->RegisterBuiltinBlocks();

    // WorldData owns the chunk data; ChunkRegistry only tracks state. Create the
    // primary terrain shell now — generated chunks land in it via the install
    // sink wired below.
    world_data_ = Create<WorldData>();
    {
        auto terrain_shell = world_data_->CreateShell(ShellCategory::kTerrain);
        primary_terrain_shell_ = terrain_shell ? terrain_shell->GetId() : kInvalidShellId;
    }

    chunk_registry_ = Create<ChunkRegistry>();
    event_bus_ = Create<EventBus>();
    meshing_context_ = ChunkMeshingContext::Create(*block_registry_);

    // Profiler: owned here as a TRef, but installed as the process-wide global
    // so worker threads (worldgen/mesh, which can't hold a ref) reach it via
    // Profiler::GetGlobal() / MI_PROF_* macros.
    profiler_ = Profiler::Create();
    Profiler::SetGlobal(profiler_.Raw());

    // Wire the streaming driver. It owns envelope/grace/dirty-set state and
    // drives the registry + meshing context from S0; the app feeds it the
    // camera chunk each tick (see StageS0/StageS2).
    streamer_ = ChunkStreamer::Create();
    streamer_->SetRegistry(chunk_registry_.Raw());
    streamer_->SetMeshingContext(meshing_context_.Raw());

    // Wire worldgens into the registry. Each ChunkGeneratorFn adapts the
    // Worldgen::GenerateChunk(shell, coord, data) virtual down to the 2-arg
    // (coord, data) signature the registry's worker calls. The shell arg is
    // nullptr: current generators (simple terrain, empty) ignore it. It exists
    // for future structure-placement generators that need shell context.
    // The fn objects are stored as app members (stable addresses); the registry
    // only holds non-owning pointers to them.
    macromc::WorldShellData* null_shell = nullptr;
    terrain_gen_fn_ = [gen = &terrain_worldgen_, null_shell](
                          const ChunkCoord& coord, ChunkData* out_data) {
        gen->GenerateChunk(null_shell, coord, out_data);
    };
    empty_gen_fn_ = [gen = &empty_worldgen_, null_shell](
                        const ChunkCoord& coord, ChunkData* out_data) {
        gen->GenerateChunk(null_shell, coord, out_data);
    };
    chunk_registry_->RegisterGenerator(ShellCategory::kTerrain, &terrain_gen_fn_);
    chunk_registry_->RegisterGenerator(ShellCategory::kEmpty, &empty_gen_fn_);

    // Wire the ownership sinks. The registry does not own chunk data — it hands
    // the freshly generated owning TRef to the install sink (which stores it in
    // the terrain shell) and keeps the returned non-owning pointer for state
    // queries. The remove sink drops the owning TRef on unload. All generated
    // chunks go to the primary terrain shell for now; structure/empty shells
    // will get their own routing later.
    chunk_registry_->SetChunkInstallSink(
        [this](const ChunkCoord& coord, ShellCategory /*category*/, mi::TRef<ChunkData> chunk) -> ChunkData* {
            if (primary_terrain_shell_ == kInvalidShellId || !world_data_) return nullptr;
            auto shell = world_data_->GetShell(primary_terrain_shell_);
            if (!shell) return nullptr;
            ChunkData* raw = chunk.Raw();
            shell->SetChunk(coord, std::move(chunk));
            return raw;
        });
    chunk_registry_->SetChunkRemoveSink(
        [this](const ChunkCoord& coord) {
            if (primary_terrain_shell_ == kInvalidShellId || !world_data_) return;
            auto shell = world_data_->GetShell(primary_terrain_shell_);
            if (shell) shell->RemoveChunk(coord);
        });

    // Set the global block atlas (shared by all GigaVoxel assets). Placeholder
    // for now; real atlas baking lands later.
    GigaVoxel::SetGlobalAtlas(BuildPlaceholderAtlas());

    // Create the game-side shell handle for the terrain shell and request its
    // render-side asset. The handle pushes commands into the render queue.
    if (primary_terrain_shell_ != kInvalidShellId && render_ctx_) {
        shell_handles_.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(primary_terrain_shell_),
            std::forward_as_tuple(primary_terrain_shell_, &render_ctx_->Queue()));
        auto it = shell_handles_.find(primary_terrain_shell_);
        if (it != shell_handles_.end()) {
            it->second.Create(ShellCategory::kTerrain);
        }
    }
}

// =============================================================================
// Initialize
// =============================================================================
void MacroMCApp::Initialize(std::unique_ptr<MIInfraInterface> infra, const MacroMCStartConfig& cfg) {
    TransferInfra(std::move(infra));
    GetInfra().Init();

    SetCurrentThreadType(ThreadType::kRenderThread);
    InitializePlatformMainThreadContext();

    glfwInit();
    InitRHI();
    InitTaskSystem();

    RDGShaderLibrary::Get().Init();

    StartWindow(cfg);

    // Swapchain.
    VkSurfaceKHR surface {};
    auto& rhi = RHI::Get();
    auto handles = static_cast<const VulkanRHIHandles*>(rhi.GetUnderlyingGraphicsAPIHandles());
    if (glfwCreateWindowSurface(handles->instance, window_, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("MacroMC: failed to create window surface");
    }
    rhi.InitializeSwapChain(&surface, cfg.window_width, cfg.window_height);

    InitRenderer();
    // TODO: configure view_ film size / camera once the renderer hookup lands
    // (part of the GigaVoxel rendering phase).

    // Wire the render-thread context. The render callback is bound to this app
    // (it calls RenderFrame + StageS3_Upload). bypass_render_thread_ comes from
    // the MACROMC_BYPASS_RENDER_THREAD CMake define.
#ifdef MACROMC_BYPASS_RENDER_THREAD
    bypass_render_thread_ = true;
#else
    bypass_render_thread_ = false;
#endif
    render_ctx_ = std::make_unique<RenderThreadContext>();
    render_ctx_->Initialize(rhi, scene_.get(), [this] {
        StageS3_Upload();   // poll completed meshes into pending_uploads_
        RenderFrame();      // paint
    }, bypass_render_thread_);

    InitWorldSubsystems();
    RegisterMacroMCCommands(*this);  // mc.profiler.dump / .reset

    MI_LOG(MIInfraLogType::kInfo, "MacroMC initialization complete.");
}

// =============================================================================
// Destroy
// =============================================================================
void MacroMCApp::Destroy() {
    auto& rhi = RHI::Get();
    // Signal gameplay/render threads to stop and join them BEFORE tearing down
    // any subsystem they touch (registry/streamer/profiler/RHI).
    stop_requested_.store(true);
    if (gameplay_thread_.joinable()) gameplay_thread_.join();
    if (render_ctx_) render_ctx_->Stop();

    rhi.WaitForIdle();

    streamer_ = nullptr;  // release first: it holds raw ptrs to the below.
    Profiler::SetGlobal(nullptr);  // workers must not reach a dangling profiler.
    profiler_ = nullptr;
    shell_handles_.clear();    // drops game-side handles (commands already flushed)
    pending_uploads_.clear();   // drop shared_ptr refs before meshing context goes
    meshes_in_flight_.clear();
    meshing_context_ = nullptr;
    event_bus_ = nullptr;
    chunk_registry_ = nullptr;  // drops non-owning ChunkData* refs
    world_data_ = nullptr;      // releases the owning TRef<ChunkData> storage

    view_.reset();
    if (scene_) scene_->ForceFlushDelayedDestruction();
    scene_.reset();

    render_ctx_.reset();  // drops the RHI sync point; RHI teardown comes next

    Renderer::DestroySingleton();

    pool_.SafeRelease();
    if (resource_allocator_) resource_allocator_->ForceFlushDelayedDestruction();
    resource_allocator_.SafeRelease();

    RDGShaderLibrary::Get().Deinit();
    TaskGraph::DestroySingleton();
    RHI::DestroySingleton();

    DestroyPlatformMainThreadContext();
    SetCurrentThreadType(ThreadType::kUnknown);

    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
    GetInfra().Shutdown();
    mi::DestroyInfra();
}

// =============================================================================
// Tick stages (TICK_PACING contract).
// =============================================================================
void MacroMCApp::TickGameplay() {
    // Time the whole gameplay tick. Individual stages also self-time.
    auto tick_scope = profiler_ ? profiler_->Scope("tick.total") : Profiler::ScopeHandle{};

    StageS0_Registry();
    StageS1_Simulation();
    StageS2_Derivation();
    // NOTE: S3 (Upload) is NOT here — it runs on the render thread via
    // RenderThreadContext::TickRender (the render callback calls StageS3_Upload
    // + RenderFrame). In bypass mode TickRender is called inline right after
    // TickGameplay by RunSingleThreaded, so S3 ordering is preserved.
    StageBoundary();   // gameplay→render command dispatch (UpdateCamera etc.)
    StageS4_LoosePhysics();

    // Frame-end profiler merge point. All workers are idle now, so it's safe
    // to fold their accumulators into the global aggregated map.
    if (profiler_) {
        if (chunk_registry_) {
            MI_PROF_GAUGE(registry_entry_count, chunk_registry_->GetEntryCount());
        }
        if (streamer_) {
            MI_PROF_GAUGE(streamer_envelope_size, streamer_->GetEnvelopeSize());
            MI_PROF_GAUGE(streamer_pending_unloads, streamer_->GetPendingUnloadCount());
            MI_PROF_GAUGE(streamer_dirty_set, streamer_->GetDirtySetSize());
        }
        MI_PROF_GAUGE(meshes_in_flight, meshes_in_flight_.size());
        MI_PROF_GAUGE(pending_uploads, pending_uploads_.size());
        profiler_->MergeFrame();
    }
}

void MacroMCApp::StageS0_Registry() {
    auto stage_scope = profiler_ ? profiler_->Scope("stage.S0_registry") : Profiler::ScopeHandle{};
    // Sync-gate A happens conceptually before S1: after this returns, the
    // chunk set + active set is stable for the tick.
    if (!chunk_registry_ || !streamer_) return;

    // Compute the camera chunk (fly camera stands in for the player for now)
    // and drive the streamer. The streamer does envelope diff, graceful
    // unload, worldgen harvest, and dirty-set production — everything that
    // does not depend on the renderer.
    ChunkCoord cam_chunk{};
    if (view_) {
        glm::ivec3 cam_block = glm::ivec3(glm::floor(view_->camera_.position));
        cam_chunk = BlockWorldToChunkCoord(cam_block);
    }
    streamer_->Tick(cam_chunk);

    // Drop region inboxes whose region has no resident chunk anymore, so the
    // EventBus doesn't accumulate inboxes forever as the player explores. This
    // is an EventBus concern (not the streamer's), so it stays in the app's S0.
    // Runs on the already-drained previous-tick state (last S1 DrainAll flushed
    // it), so no live events are lost. See EventBus::GC.
    if (event_bus_) {
        auto live_regions = chunk_registry_->GetResidentRegions();
        event_bus_->GC(live_regions);
    }
}

void MacroMCApp::StageS1_Simulation() {
    auto stage_scope = profiler_ ? profiler_->Scope("stage.S1_simulation") : Profiler::ScopeHandle{};
    // Checkerboard gameplay tick. Drains events from the bus at start, emits
    // new events during the tick. Stub for now.
    if (event_bus_) {
        (void)event_bus_->DrainAll();  // TODO: dispatch to active chunks.
    }
}

void MacroMCApp::StageS2_Derivation() {
    auto stage_scope = profiler_ ? profiler_->Scope("stage.S2_derivation") : Profiler::ScopeHandle{};
    // Parallel mesh rebuild over the dirty set (produced by the streamer in S0).
    // For each dirty chunk: ensure it is registered with the meshing context,
    // then request a (re-)mesh. The mesh task captures TRef snapshots of the
    // centre + 8 neighbours and runs on a worker (see chunk_meshing_context.h).
    // Single-threaded dispatch here (S2 is on the render/main thread); only the
    // mesh compute is parallel.
    if (!chunk_registry_ || !meshing_context_ || !streamer_) return;
    auto dirty = streamer_->ConsumeDirtySet();
    if (dirty.empty()) return;

    ChunkCoord cam_chunk;
    if (view_) {
        glm::ivec3 cam_block = glm::ivec3(glm::floor(view_->camera_.position));
        cam_chunk = BlockWorldToChunkCoord(cam_block);
    }

    for (const auto& coord : dirty) {
        // Find() returns a non-owning presence check (the registry does not own
        // chunk data). For meshing we need an owning TRef lease — get it from
        // the owning container (WorldShellData) so neighbour snapshots stay
        // valid for the mesh worker's lifetime.
        if (!chunk_registry_->Find(coord)) continue;  // became unloaded; skip.
        mi::TRef<ChunkData> chunk;
        if (world_data_ && primary_terrain_shell_ != kInvalidShellId) {
            auto shell = world_data_->GetShell(primary_terrain_shell_);
            if (shell) chunk = shell->GetChunk(coord);
        }
        if (!chunk) continue;
        // Register first (no-op if already registered). Registering on ready
        // gives the meshing context a TRef lease so neighbour snapshots can
        // include this chunk.
        meshing_context_->RegisterChunk(coord, chunk);
        meshing_context_->RequestMesh(coord, cam_chunk);
        // Track for S3: poll GetResult on this coord until the mesh completes.
        meshes_in_flight_.insert(coord);
    }
}

void MacroMCApp::StageS3_Upload() {
    auto stage_scope = profiler_ ? profiler_->Scope("stage.S3_upload") : Profiler::ScopeHandle{};
    // Poll in-flight meshes; move completed ones into the pending-upload queue.
    // GetResult returns null until the worker sets the done flag; null ones stay
    // in-flight for next tick. This is non-blocking — S3 never waits.
    if (meshing_context_) {
        for (auto it = meshes_in_flight_.begin(); it != meshes_in_flight_.end();) {
            auto result = meshing_context_->GetResult(*it);
            if (result) {
                pending_uploads_[*it] = std::move(result);
                it = meshes_in_flight_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Flatten + upload completed meshes into the GigaVoxel asset. This runs on
    // the render thread (StageS3 is called from the render callback), so we go
    // directly through the shell registry rather than the SPSC command queue
    // (the queue is for game→render; render-side data stays render-side).
    // Meshing results are only readable on the render/main thread (the meshing
    // context's chunks_ map is single-threaded), so flattening happens here.
    if (!pending_uploads_.empty() && render_ctx_ && primary_terrain_shell_ != kInvalidShellId) {
        auto & registry = render_ctx_->ShellRegistry();
        for (auto it = pending_uploads_.begin(); it != pending_uploads_.end();) {
            const ChunkCoord & coord = it->first;
            auto flat = FlattenChunkMesh(it->second);
            it = pending_uploads_.erase(it);
            if (flat.vertices.empty() || flat.indices.empty()) continue;
            UploadChunkMeshCmd cmd;
            cmd.shell = primary_terrain_shell_;
            cmd.coord = coord;
            cmd.vertices = std::move(flat.vertices);
            cmd.indices = std::move(flat.indices);
            registry.HandleCommand(cmd);
        }
    }
    // Drop entries for chunks that were unloaded (grace-flushed in S0) so the
    // in-flight/pending sets don't leak stale coords.
    if (chunk_registry_) {
        for (auto it = meshes_in_flight_.begin(); it != meshes_in_flight_.end();) {
            if (!chunk_registry_->Find(*it)) it = meshes_in_flight_.erase(it);
            else ++it;
        }
    }
}

void MacroMCApp::StageBoundary() {
    auto stage_scope = profiler_ ? profiler_->Scope("stage.boundary") : Profiler::ScopeHandle{};
    // The cross-thread handoff point (THREAD_MODEL.md §6): push the gameplay-
    // side state the render thread needs into the command queue. At minimum,
    // an UpdateCamera each tick so the render-side camera snapshot tracks the
    // player. (Future: UploadChunkMesh for newly-meshed chunks, DestroyChunkMesh
    // for unloaded chunks — wired in the GigaVoxel phase.)
    // In bypass mode EnqueueCommand runs inline, so this is a no-cost direct call.
    if (render_ctx_) {
        render_ctx_->EnqueueCommand(MakeCameraCommand());
    }
}

void MacroMCApp::StageS4_LoosePhysics() {
    auto stage_scope = profiler_ ? profiler_->Scope("stage.S4_loose_physics") : Profiler::ScopeHandle{};
    // Loose physics steps into N+1; reads committed VoxelShape/body snapshot.
    // Emits physics events back into the bus for next tick's S1. Stub.
}

// =============================================================================
// RenderFrame — placeholder. The real graph (clear backbuffer + Renderer::Render
// + ImGui overlay) lands with the GigaVoxel rendering phase; for now the loop
// only advances the frame so the window stays responsive.
// =============================================================================
void MacroMCApp::RenderFrame() {
    if (!view_ || !scene_ || !render_ctx_) return;

    // Apply the latest render-side camera snapshot (updated from UpdateCameraCmd
    // in DrainCommands) to the view's camera, so the renderer paints from it
    // rather than the gameplay thread's live camera.
    auto & snap = render_ctx_->Camera();
    view_->camera_.position = snap.position;
    view_->camera_.direction = snap.forward;
    view_->camera_.up = snap.up;
    view_->camera_.fov_Y = snap.fov_y_rad;

    RenderGraphBuilder builder;
    auto backbuffer = builder.Import(RHI::Get().GetBackBuffer());
    // Clear backbuffer.
    builder.AddPass("ClearBackBuffer", RDGPassType::kGeneric, {}, {}, {}, {},
        [bf = backbuffer]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            queue.ClearTexture(bf->GetRHI(), {0, 0, 0, 1});
        })->AddTextureH(backbuffer, RDGTextureUsageType::kTransferWrite);

    // Render the scene (adds the GigaVoxel per-chunk BLAS build pass + the
    // standard deferred/ray-tracing passes into the builder).
    Renderer::Get().Render(view_.get(), builder);

    auto graph = builder.Compile("MacroMCFrame");
    graph->Execute(pool_.Raw());
}

// =============================================================================
// Run — dispatches to single- or multi-threaded mode based on bypass flag.
// =============================================================================
void MacroMCApp::Run() {
    if (bypass_render_thread_) {
        RunSingleThreaded();
    } else {
        RunMultiThreaded();
    }
}

// -----------------------------------------------------------------------------
// Bypass mode: everything on the main thread, serially. glfw → gameplay tick
// (fixed timestep accumulator) → render TickRender (which does S3 + RenderFrame
// + AdvanceFrame). This is the debug-friendly path: no concurrency, all RHI
// asserts see the single thread as kRenderThread.
// -----------------------------------------------------------------------------
void MacroMCApp::RunSingleThreaded() {
    float cpu_duration = 0.0f;
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        auto frame_start = std::chrono::steady_clock::now();

        // Fixed-timestep simulation: catch up the world in kFixedTickDt steps.
        tick_accumulator_ += static_cast<double>(cpu_duration);
        if (tick_accumulator_ > kFixedTickDt * 5.0) {
            tick_accumulator_ = kFixedTickDt * 5.0;  // clamp: avoid spiral-of-death
        }
        while (tick_accumulator_ >= kFixedTickDt) {
            TickGameplay();       // S0/S1/S2/boundary/S4 (boundary pushes cmds inline)
            tick_accumulator_ -= kFixedTickDt;
        }

        // Render side: drains the (inline-applied) command queue + paints +
        // advances the frame. In bypass this is all on the main thread.
        if (render_ctx_) render_ctx_->TickRender();

        auto frame_end = std::chrono::steady_clock::now();
        cpu_duration = std::chrono::duration<float>(frame_end - frame_start).count();
    }
}

// -----------------------------------------------------------------------------
// Multi-threaded mode: main thread only does glfw + input handoff. The gameplay
// thread (GameplayThreadLoop) runs fixed-TPS ticks; the render thread
// (RenderThreadContext::RenderThreadLoop) runs free-FPS rendering. They
// communicate solely through the render command queue.
// -----------------------------------------------------------------------------
void MacroMCApp::RunMultiThreaded() {
    // The main thread was marked kRenderThread during Initialize (RHI init needs
    // that identity). Now it hands the render identity to the real render thread
    // and becomes input/window-only — mark it kUnknown so any stray RHI call
    // from main surfaces immediately rather than silently passing IsRenderThread.
    mi::SetCurrentThreadType(mi::ThreadType::kUnknown);

    // Start the gameplay + render threads.
    gameplay_thread_ = std::thread([this] { GameplayThreadLoop(); });
    render_ctx_->Start();

    // Main loop: glfw + a minimal input handoff to gameplay.
    // (Full input routing — key/mouse state → gameplay — lands with the input
    // system. For now the gameplay thread reads view_->camera_ directly since
    // in this skeleton the fly-camera controller still runs where it always did;
    // real decoupling of input ownership is a follow-up.)
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        std::this_thread::yield();  // don't spin; glfwPollEvents isn't blocking
    }

    // Shutdown: signal threads, join, then Destroy() does the subsystem teardown.
    stop_requested_.store(true);
    if (gameplay_thread_.joinable()) gameplay_thread_.join();
    render_ctx_->Stop();
}

// -----------------------------------------------------------------------------
// Gameplay thread body: fixed 20 TPS loop running TickGameplay. Never touches
// the RHI — all rendering intent flows through the command queue.
// -----------------------------------------------------------------------------
void MacroMCApp::GameplayThreadLoop() {
    mi::SetCurrentThreadType(mi::ThreadType::kGameplayThread);
    InitializePlatformBackgroundThreadContext_Worker();

    constexpr auto kTickInterval = std::chrono::milliseconds(50);  // 20 TPS
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        auto tick_start = std::chrono::steady_clock::now();
        TickGameplay();
        // Sleep the remainder of the tick interval to hold 20 TPS. If the tick
        // overran the interval, don't sleep (we're behind) — but the loop won't
        // spiral because TickGameplay is bounded (no blocking waits).
        auto elapsed = std::chrono::steady_clock::now() - tick_start;
        if (elapsed < kTickInterval) {
            std::this_thread::sleep_for(kTickInterval - elapsed);
        }
    }

    mi::SetCurrentThreadType(mi::ThreadType::kUnknown);
}

UpdateCameraCmd MacroMCApp::MakeCameraCommand() const {
    UpdateCameraCmd cmd;
    if (view_) {
        cmd.position = view_->camera_.position;
        cmd.forward = view_->camera_.direction;   // Camera uses `direction`
        cmd.up = view_->camera_.up;
        cmd.fov_y_rad = view_->camera_.fov_Y;     // Camera uses `fov_Y` (radians)
    }
    return cmd;
}

MACROMC_NAMESPACE_END
