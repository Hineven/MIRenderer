#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <limits>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "viewer_console.h"
#include "infra_impl/infra.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_resource_allocator.h"
#include "rdg/rdg.h"

struct GLFWwindow;

MI_NAMESPACE_BEGIN

struct MainLoopStartConfig {
    std::string window_name;
    uint32_t window_width;
    uint32_t window_height;
};

class ViewerApp {
public:
    struct SelectionState {
        uint selected_renderable_index = UINT32_MAX;
        uint selected_primitive_index = UINT32_MAX;
        uint selected_descriptor_rank = UINT32_MAX;
        uint selected_deferred_renderable_index = UINT32_MAX;
        glm::vec2 selected_uv {0.0f, 0.0f};
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
        bool should_export_result {};
        bool should_start_baking {};
        bool should_process_click_select {};
        bool should_export_baking_result {};
    };

    struct PerfStat {
        float min_ms = std::numeric_limits<float>::infinity();
        float max_ms = 0.0f;
    };


    void Initialize(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg);
    void Destroy();
    void LoadScene(const MainLoopStartConfig& cfg);
    void HandleNavigationInput(float delta_time);
    void HandleKeyboardShortcuts(FrameInternalDelayedOps& ops);
    void HandleControlUILogic(FrameInternalDelayedOps& ops, std::vector<RDGTimePeriod> time_periods, float cpu_duration);

    void ProcessClickSelect(FrameInternalDelayedOps& ops);
    void ProcessDelayedOps(FrameInternalDelayedOps& ops);
    void ProcessAxisDragging();
    void Run(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg);

    static uint32_t GetConsoleTextColor(MIInfraLogType type);

    GLFWwindow * window_ {};

    TRef<RHITexture> font_texture_;
    TRef<Texture> sky_cube_;
    TRef<RDGResourcePool> pool_;
    TRef<DeviceBindlessResourceAllocator> resource_allocator_;
    std::unique_ptr<RendererView> view_;
    std::unique_ptr<Scene> scene_;

    TRef<Material> default_material_;

    std::vector<TRef<StaticMeshInstance>> meshes_;

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
};

// Entry point for running the 3d viewer main loop.
void Run3DViewer(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg);


MI_NAMESPACE_END

