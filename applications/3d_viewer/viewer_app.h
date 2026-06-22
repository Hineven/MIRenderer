#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <limits>
#include <future>
#include <mutex>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include "util/renderable_node.h"

#include "co_wrapper.h"
#include "viewer_console.h"
#include "infra_impl/infra.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_giga_voxel.h"
#include "renderer/mi_resource_allocator.h"
#include "rdg/rdg.h"
#include "viewer_zmq.h"

struct GLFWwindow;

MI_NAMESPACE_BEGIN

struct MainLoopStartConfig {
    std::string window_name;
    uint32_t window_width;
    uint32_t window_height;
    std::string scene_config_path;
    bool start_empty = false;
};

// A user-saved camera position that can be persisted across runs and
// recalled later. Higher priority entries take precedence on startup.
struct PersistentCamera {
    std::string name;
    int priority {0};
    Camera camera {};
};

class ViewerApp {
public:
    struct SelectionState {
        uint selected_renderable_index = UINT32_MAX;
        uint selected_primitive_index = UINT32_MAX;
        uint selected_descriptor_rank = UINT32_MAX;
        uint selected_deferred_renderable_index = UINT32_MAX;
        glm::vec2 selected_uv {0.0f, 0.0f};
        // Used to identify if the user intention is dragging or clicking.
        bool mouse_moved_since_pressed {false};
    };

    struct BakingState {
        uint32_t baking_max_num_frames {1024};

        bool is_baking_mode = false;
        std::vector<Camera> baking_camera_positions;
        uint32_t baking_frame_index = 0;
        uint32_t baking_camera_index = 0;

        void ClearBakingState();
    };

    struct InputState {
        bool dragging_ {};
        glm::vec2 drag_mouse_start_pos_ {};
        glm::vec3 drag_start_obj_pos_ {};
        float last_click_forward_depth_ {};
    };

    struct FrameInternalDelayedOps {
        bool should_reload_shaders {};
        bool did_reload_shaders {};
        bool should_export_result {};
        bool should_start_baking {};
        bool should_process_click_select {};
        bool should_export_baking_result {};
    };

    struct LoadedScene {
        std::string name;
        std::vector<TRef<RenderableNode>> roots;
    };

    struct PerfStat {
        float min_ms = std::numeric_limits<float>::infinity();
        float max_ms = 0.0f;
    };


    void Initialize(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg);
    void Destroy();
    void LoadScene(const MainLoopStartConfig& cfg);
    bool LoadSceneFromConfigAbsolutePath(const std::filesystem::path& scene_config_path, std::string* out_error = nullptr, bool clear_existing = true);
    bool LoadSceneFromConfigJsonString(const std::string& scene_json_str, std::string* out_error = nullptr, bool clear_existing = true);
    bool ApplySceneConfig(const nlohmann::json& scene_config, bool clear_existing);
    void HandleNavigationInput(float delta_time);
    void HandleKeyboardShortcuts(FrameInternalDelayedOps& ops);
    void HandleControlUILogic(FrameInternalDelayedOps& ops, std::vector<RDGTimePeriod> time_periods, float cpu_duration);

    void ProcessClickSelect(FrameInternalDelayedOps& ops);
    void ProcessDelayedOps(FrameInternalDelayedOps& ops, const TRef<RendererExports>& exports);
    void ProcessAxisDragging();
    void SetSelectedRenderable(Renderable* renderable);
    void RegisterLoadedScene(const std::string& name, const std::vector<TRef<RenderableNode>>& roots);

    // Persistent camera management.
    // All of these operate on the in-memory persistent_cameras_ list and (where appropriate)
    // immediately persist the list back to viewer_app_config.json.
    void SaveCurrentCameraAsPersistent(const std::string& name, int priority);
    void DeletePersistentCamera(size_t index);
    void UpdatePersistentCamera(size_t index);
    void ApplyPersistentCamera(size_t index);
    void MovePersistentCameraPriority(size_t index, int delta);
    void RenamePersistentCamera(size_t index, const std::string& new_name);
    // Serialize persistent_cameras_ into the in-memory config snapshot and write to disk.
    void SavePersistentCamerasToConfig();
    // Load persistent_cameras_ from a config json snapshot.
    void LoadPersistentCamerasFromConfig(const nlohmann::json& config);
    // Write persistent_cameras_ into the "persistent_cameras" key of `out` only.
    void SerializePersistentCamerasToJson(nlohmann::json& out) const;

    void WaitForSceneMutation();
    void FlushSceneDelayedDestruction();
    void UnloadScene(size_t idx);
    void Run(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg);

    void EnqueueNextFrameOperations (std::function<void()> func);
    std::vector<std::function<void()>> next_frame_operations_;

    // ZMQ server ops

    struct ViewerStatus {
        Camera camera {};
        uint32_t frame_index {};
        bool is_suspended {};
    };
    ViewerStatus GetStatus () ;
    std::vector<ViewerImGuiConsole::ConsoleLogEntry> GetLatestUniqueLogs(size_t max_count) const;
    struct ExportedRenderResult {
        std::vector<std::byte> bytes;
        uint32_t width, height;
        PixelFormatType format;
        std::string name;
    };
    std::vector<ExportedRenderResult> GetAndClearExportedFrameResults ();

    bool LoadGLTFAbsolute(const std::filesystem::path& path, std::vector<uint32_t>* out_renderable_indices = nullptr);
    bool LoadEnvironmentMapAbsolute(const std::filesystem::path& path);
    bool RemoveRenderableNodeByIndex(uint32_t renderable_node_index);
    bool CleanAllRenderableNodes();

    inline bool IsSuspended() const { return suspended_; }
    inline void SetSuspended(bool v) { suspended_ = v; }

    GLFWwindow * window_ {};

    TRef<RHITexture> font_texture_;
    TRef<Texture> sky_cube_;
    TRef<RDGResourcePool> pool_;
    TRef<DeviceBindlessResourceAllocator> resource_allocator_;
    std::unique_ptr<RendererView> view_;
    std::unique_ptr<Scene> scene_;

    TRef<Material> default_material_;

    TRef<RenderableNodeRegistry> renderable_node_registry_;

    std::vector<LoadedScene> loaded_scenes_;

    TRef<StaticMeshInstance> arrow_mesh_x_instance_;
    TRef<StaticMeshInstance> arrow_mesh_y_instance_;
    TRef<StaticMeshInstance> arrow_mesh_z_instance_;
    TRef<StaticMesh> arrow_mesh_x_;
    TRef<StaticMesh> arrow_mesh_y_;
    TRef<StaticMesh> arrow_mesh_z_;
    TRef<Geometry> arrow_geometry_;

    SelectionState selection_state_ {};
    BakingState baking_state_ {};
    InputState input_state_ {};

    ViewerImGuiConsole console_;

    std::unordered_map<std::string, PerfStat> perf_stats_;

    std::vector<CVarBase *> pinned_cvars_;

    // Persistent camera positions, loaded from / saved to viewer_app_config.json.
    std::vector<PersistentCamera> persistent_cameras_;
    // In-memory snapshot of viewer_app_config.json so that any subsystem
    // (e.g. persistent cameras) can update only its own key and re-save the
    // whole file without clobbering other subsystems' state.
    nlohmann::json config_json_;

    // Temporarily keep some of the exported results for ZMQ server to use.
    std::vector<ExportedRenderResult> exported_render_results_;
    TRef<RDGTexture> tonemapped_color_export_;
    TRef<RDGTexture> tonemapped_path_tracing_export_;

    // ZMQ server for Python integration
    std::unique_ptr<ViewerZmqServer> zmq_server_;

    // Suspended mode: when true, the render loop skips Renderer::Render.
    // A single frame render can be triggered by setting request_one_render_ = true.
    std::atomic<bool> suspended_{false};
    std::atomic<bool> one_frame_rendering_requested_{false};

    // Export frame handshake between ZMQ thread and render loop.
    std::atomic<bool> export_frame_request_{false};
};

// Entry point for running the 3d viewer main loop.
void Run3DViewer(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg);

// Some helpers

// Maps viewer export names to RendererExports string IDs.
// For tonemapped exports, the source_texture_id identifies the source RDG texture
// that will be fed into the tonemapping pass.
struct ViewerFrameExportBinding {
    const char* export_id;
    const char* source_texture_id; // for tonemapped exports: source texture in RendererExports
    PixelFormatType format {PixelFormatType::kUnknown};
    bool is_tonemapped {false};
};
const ViewerFrameExportBinding* FindViewerFrameExportBinding(std::string_view name) ;
const std::vector<std::string>& GetSupportedViewerFrameExportNames() ;

MI_NAMESPACE_END

