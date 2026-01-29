#include "viewer_app.h"

#include <fstream>
#include <algorithm>
#include "vulkan/vulkan.hpp"
#include <glfw/glfw3.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include "imgui_impl_glfw.h"

#include "rhi/vk/vk_export.h"
#include "rdg/rdg_pool.h"
#include "rhi/rhi_buffer.h"
#include "rhi/rhi_texture.h"
#include "core/util/debug_prof.h"
#include "core/task.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_cvar.h"
#include "renderer/r_geometry_buffer.h"

// Okay, some internal headers for the renderer. We need these for various tasks.
#include "../../renderer/renderer/r_volume_direct_lighting.h"
#include "../../renderer/renderer/r_volume_indirect_lighting.h"
#include "../../renderer/renderer/r_volume_primitives.h"
#include "../../renderer/renderer/r_persistent.h"
#include "../../renderer/renderer/r_gaussian_radiance_field.h"

#include "rdg/rdg_shader.h"
#include "util/texture_loader.h"
#include "util/gltf_loader.h"
#include "util/volprims_loader.h"
#include "util/gaussian_radiance_field_loader.h"

#include "3d_viewer.h"
#include "core/util/command_line.h"
#include "viewer_zmq.h"
#include "viewer_commands.h"
#include "viewer_control.h"
#include "renderer/mi_renderer_view.h"
#include "core/pixel_format.h"

MI_NAMESPACE_BEGIN

// Generic helper: read back an RDG texture into CPU bytes.
// Returns true on success and fills out parameters.
static bool ReadbackRDGTextureToBytes(
    RDGTexture* rdg_tex,
    PixelFormatType fmt,
    std::vector<std::byte>& out_bytes,
    uint32_t& out_w,
    uint32_t& out_h
) {
    if (!rdg_tex) return false;
    auto tex = rdg_tex->GetRHI();
    if (!tex) return false;

    auto& rhi = RHI::Get();
    auto& queue = rhi.GetGraphicsCommandQueue();

    const uint32_t w = tex->GetWidth();
    const uint32_t h = tex->GetHeight();
    const size_t bpp = GetPixelFormatBytesPerPixel(fmt);
    const size_t byte_size = size_t(w) * size_t(h) * bpp;

    auto readback = rhi.CreateBuffer(byte_size, RHIBufferUsageFlagBits::kReadback);

    queue.MemoryBarrier();
    queue.TextureBarrier(tex, RHITextureLayoutType::kTransferSrcOptimal,
        RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
        RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
    queue.CopyTextureToBuffer(tex, readback.Raw());

    // Mark usage for RDG lifetime/alias tracking as in existing patterns
    rdg_tex->Use(
        RHIPipelineStageFlagBits::kTransfer,
        RHIGPUAccessFlagBits::kTransferRead,
        RHITextureLayoutType::kTransferSrcOptimal
    );

    queue.MemoryBarrier();
    queue.WaitForIdle("Readback RDGTexture");

    out_bytes.resize(byte_size);
    void* ptr = readback->Map();
    if (ptr && byte_size > 0) {
        std::memcpy(out_bytes.data(), ptr, byte_size);
    }
    readback->Unmap();

    out_w = w;
    out_h = h;
    return true;
}

static std::string GetCurrentDateTimeString();
static std::string GenerateRandomString(uint32_t len = 4);
static GLFWwindow* StartWindow(const MainLoopStartConfig& cfg);

void ViewerApp::BakingState::ClearBakingState() {
    baking_camera_positions.clear();
    baking_camera_index = 0;
    baking_frame_index = 0;
    is_baking_mode = false;
}

static std::string GetCurrentDateTimeString() {
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &tm);
    return std::string(buffer);
}

static std::string GenerateRandomString(uint32_t len) {
    const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    std::string str;
    str.resize(len);
    for (uint32_t i = 0; i < len; i++) {
        str[i] = charset[rand() % max_index];
    }
    return str;
}

static GLFWwindow* StartWindow(const MainLoopStartConfig& cfg) {
    if (!glfwInit()) {
        mi_assert(false, "Failed to initialize GLFW");
        return nullptr;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    std::string window_name = "MIRenderer";
    if (!cfg.window_name.empty()) window_name = cfg.window_name;
    GLFWwindow* window = glfwCreateWindow(
        cfg.window_width, cfg.window_height, window_name.c_str(),
        nullptr, nullptr
    );
    if (!window) {
        glfwTerminate();
        mi_assert(false, "Failed to create GLFW window");
        return nullptr;
    }

    return window;
}

static std::filesystem::path GetViewerAppConfigPath() {
    // Save next to executable.
    // We avoid Infra FS helpers as requested; using current_path() works when the process
    // is launched from its executable directory (the common case for this app).
    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    if (ec) dir = std::filesystem::path(".");
    return dir / "viewer_app_config.json";
}

static void LoadConfig(nlohmann::json &out_config) {
    const auto path = GetViewerAppConfigPath();
    std::ifstream ifs(path);
    if (!ifs) {
        MI_LOG(MIInfraLogType::kInfo, "ViewerApp config not found: {}", path.string());
        out_config = {};
        return;
    }
    try {
        ifs >> out_config;
    } catch (const std::exception &e) {
        MI_WARN("Failed to read ViewerApp config {}: {}", path.string(), e.what());
        out_config = {};
    }
}

static void LoadPinnedCVarsFromConfig(const nlohmann::json &config, std::vector<CVarBase *> &out_pinned) {
    out_pinned.clear();

    if (!config.contains("pinned_cvars") || !config["pinned_cvars"].is_array()) {
        MI_WARN("ViewerApp config malformed: missing pinned_cvars array");
        return;
    }

    auto &reg = CVarRegistry::GetInstance();
    for (const auto &idJson : config["pinned_cvars"]) {
        if (!idJson.is_string()) continue;
        const std::string id = idJson.get<std::string>();
        if (id.empty()) continue;
        if (auto *cvar = reg.GetCVar(id)) {
            out_pinned.push_back(cvar);
        } else {
            MI_WARN("ViewerApp config: pinned cvar '{}' no longer exists", id);
        }
    }

    MI_LOG(MIInfraLogType::kInfo, "Loaded {} pinned cvars", out_pinned.size());
}

static void SaveConfig(const nlohmann::json &config) {
    const auto path = GetViewerAppConfigPath();
    try {
        std::ofstream ofs(path, std::ios::out | std::ios::trunc);
        if (!ofs) {
            MI_WARN("Failed to write ViewerApp config: {}", path.string());
            return;
        }
        ofs << std::setw(4) << config << std::endl;
        MI_LOG(MIInfraLogType::kInfo, "Saved ViewerApp config: {}", path.string());
    } catch (const std::exception &e) {
        MI_WARN("Failed to save ViewerApp config {}: {}", path.string(), e.what());
    }
}

void ViewerApp::Initialize(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg) {
    auto pwd = std::filesystem::current_path();

    TransferInfra(std::move(infra));
    GetInfra().Init();

    // Load persisted UI state.
    nlohmann::json json_config;
    LoadConfig(json_config);
    LoadPinnedCVarsFromConfig(json_config, pinned_cvars_);
    console_.Initialize(json_config);

    RegisterViewerCommands(*this);

    if (GetCurrentThreadType() != ThreadType::kUnknown) {
        mi_assert(false, "MainLoop: somehow the thread calling Start() is known.");
    }
    SetCurrentThreadType(ThreadType::kRenderThread);
    InitializePlatformMainThreadContext();

    glfwInit();

    {
        uint32_t extension_count = 0;
        auto extra_extensions = glfwGetRequiredInstanceExtensions(&extension_count);
        if (extension_count != 0) {
            VulkanRHICreateInfo info {};
            info.extra_instance_extension_count = extension_count;
            info.extra_instance_extensions = extra_extensions;
            RHI::InitializeSingleton(RHIType::kVulkan, &info);
            RHI::Get().ResetPipelineCache(4 * 1024 * 1024);
        } else {
            throw std::runtime_error("Failed to get required instance extensions");
        }
    }

    auto limits = GetInfra().GetResourceLimits();

    if (limits.max_high_performance_thread_count < 2) {
        mi_assert(false, "At least 2 high performance threads are required.");
    }
    int task_graph_hpt_count = std::max(limits.max_high_performance_thread_count - 2, 0);
    int total_task_graph_thread_count = task_graph_hpt_count + limits.max_low_performance_thread_count;
    if (total_task_graph_thread_count < 1) {
        mi_assert(false, "At least 3 threads are required.");
    }

    TaskGraph::InitializeSingleton(0, task_graph_hpt_count);

    auto& shader_lib = RDGShaderLibrary::Get();
    shader_lib.Init();

    MI_LOG(MIInfraLogType::kInfo, "Main loop initialization complete.");

    window_ = StartWindow(cfg);

    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForVulkan(window_, true);
        unsigned char* pixels;
        int width, height;
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        auto& rhi = RHI::Get();
        font_texture_ = rhi.CreateTexture(RHITextureType::k2D,
            {(uint32_t)width, (uint32_t)height, 1},
            PixelFormatType::kR8G8B8A8_UNORM,
            RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst);
        auto staging = rhi.CreateBuffer(width * height * 4, RHIBufferUsageFlagBits::kStaging);
        memcpy(staging->Map(), pixels, width * height * 4);
        auto& cmd = rhi.GetGraphicsCommandQueue();
        cmd.TextureBarrier(font_texture_.Raw(), RHITextureLayoutType::kTransferDstOptimal,
            RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kWrite);
        cmd.CopyBufferToTexture(staging->GetSpan(), font_texture_.Raw());
        cmd.TextureBarrier(font_texture_.Raw(), RHITextureLayoutType::kShaderReadOnlyOptimal,
            RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kWrite, RHIGPUAccessFlagBits::kRead);
        cmd.EnqueueTranslateAndSubmit();
        rhi.WaitForIdle();
        io.Fonts->SetTexID((ImTextureID)font_texture_.Raw());
        io.Fonts->ClearTexData();
    }

    VkSurfaceKHR surface_tmp;
    auto& rhi = RHI::Get();
    auto handles = static_cast<const VulkanRHIHandles*>(rhi.GetUnderlyingGraphicsAPIHandles());
    {
        auto result = glfwCreateWindowSurface(handles->instance, window_, nullptr, &surface_tmp);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create window surface");
        }
    }
    rhi.InitializeSwapChain(&surface_tmp, cfg.window_width, cfg.window_height);

    pool_ = RDGResourcePool::Create();

    resource_allocator_ = Create<DeviceBindlessResourceAllocator>();

    Renderer::Get().Init(resource_allocator_.Raw(), pool_.Raw());

    // Initialize ZMQ server.
    ViewerZmqServer::Config zc {};
    zmq_server_ = std::make_unique<ViewerZmqServer>(this, zc);
    zmq_server_->Initialize();

}

void ViewerApp::Destroy() {

    zmq_server_.reset();

    // Persist UI state before tearing subsystems down.
    nlohmann::json json_config;
    json_config["pinned_cvars"] = nlohmann::json::array();
    for (auto *cvar : pinned_cvars_) {
        if (!cvar) continue;
        json_config["pinned_cvars"].push_back(cvar->GetId());
    }

    default_material_.SafeRelease();
    {
        arrow_mesh_x_instance_.SafeRelease();
        arrow_mesh_y_instance_.SafeRelease();
        arrow_mesh_z_instance_.SafeRelease();
        arrow_mesh_x_.SafeRelease();
        arrow_mesh_y_.SafeRelease();
        arrow_mesh_z_.SafeRelease();
        arrow_geometry_.SafeRelease();
    }

    view_.reset();

    loaded_scenes_.clear();
    renderable_node_lookup_.clear();

    scene_.reset();

    sky_cube_.SafeRelease();

    Renderer::DestroySingleton();

    assert(pool_.GetRefCount() == 1);
    pool_.SafeRelease();

    resource_allocator_.SafeRelease();

    RDGShaderLibrary::Get().Deinit();

    TaskGraph::DestroySingleton();

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    font_texture_.SafeRelease();

    RHI::DestroySingleton();

    console_.Destroy(json_config);
    SaveConfig(json_config);

    DestroyPlatformMainThreadContext();
    SetCurrentThreadType(ThreadType::kUnknown);

    GetInfra().Shutdown();
    DestroyInfra();

    glfwTerminate();
}

void ViewerApp::HandleNavigationInput(float delta_time) {
    const float move_speed = 1.f;
    const float mouse_sensitivity = 0.002f;
    float dt = delta_time;
    float move_interval = move_speed * dt;
    glm::vec3 camera_right = glm::normalize(glm::cross(view_->camera_.direction, glm::vec3(0.0f, 1.0f, 0.0f)));
    if (!ImGui::GetIO().WantCaptureKeyboard) {
        if (ImGui::IsKeyDown(ImGuiKey_W))
            view_->camera_.position += view_->camera_.direction * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_S))
            view_->camera_.position -= view_->camera_.direction * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_A))
            view_->camera_.position -= camera_right * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_D))
            view_->camera_.position += camera_right * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_Space))
            view_->camera_.position += view_->camera_.up * move_interval;
        if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
            view_->camera_.position -= view_->camera_.up * move_interval;
    }

    static double last_mouse_x = 0.0, last_mouse_y = 0.0;
    static bool first_mouse = true;

    double mouse_x, mouse_y;
    glfwGetCursorPos(window_, &mouse_x, &mouse_y);

    if (first_mouse) {
        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
        first_mouse = false;
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !input_state_.dragging_ && !ImGui::GetIO().WantCaptureMouse) {
        float delta_x = static_cast<float>(mouse_x - last_mouse_x) * mouse_sensitivity;
        float delta_y = static_cast<float>(mouse_y - last_mouse_y) * mouse_sensitivity;

        glm::mat4 rotate_y = glm::rotate(glm::mat4(1.0f), -delta_x, glm::vec3(0, 1, 0));
        view_->camera_.direction = glm::vec3(rotate_y * glm::vec4(view_->camera_.direction, 0.0f));

        glm::mat4 rotate_x = glm::rotate(glm::mat4(1.0f), -delta_y, camera_right);
        view_->camera_.direction = glm::vec3(rotate_x * glm::vec4(view_->camera_.direction, 0.0f));

        view_->camera_.direction = glm::normalize(view_->camera_.direction);
    }

    last_mouse_x = mouse_x;
    last_mouse_y = mouse_y;
}

void ViewerApp::HandleKeyboardShortcuts(FrameInternalDelayedOps& ops) {
    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
        ops.should_reload_shaders = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        auto dir = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/camera_configs/");
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
        auto date_time = GetCurrentDateTimeString();
        auto random_str = GenerateRandomString(6);
        auto file_path = dir / (date_time + random_str + ".json");
        nlohmann::json j;
        j["camera_position"] = {view_->camera_.position.x, view_->camera_.position.y, view_->camera_.position.z};
        j["camera_direction"] = {view_->camera_.direction.x, view_->camera_.direction.y, view_->camera_.direction.z};
        j["camera_fov_Y"] = view_->camera_.fov_Y;
        j["camera_near_plane"] = view_->camera_.near_plane;
        j["camera_far_plane"] = view_->camera_.far_plane;
        j["camera_up"] = {view_->camera_.up.x, view_->camera_.up.y, view_->camera_.up.z};
        std::ofstream o(file_path);
        o << std::setw(4) << j << std::endl;
        MI_LOG(MIInfraLogType::kInfo, "Saved camera config to {}.", file_path.string());
    }

    if (ImGui::IsKeyPressed(ImGuiKey_E)) {
        ops.should_export_result = true;
    }
}

void ViewerApp::HandleControlUILogic(FrameInternalDelayedOps& ops, std::vector<RDGTimePeriod> time_periods, float cpu_duration) {
    ViewerControlUI::DrawControlUI(*this, ops, std::move(time_periods), cpu_duration);
}

void ViewerApp::ProcessClickSelect(FrameInternalDelayedOps& ops) {
    auto & io = ImGui::GetIO();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.MouseDelta.x * io.MouseDelta.x + io.MouseDelta.y * io.MouseDelta.y > 0.3f) {
        selection_state_.mouse_moved_since_pressed = true;
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !selection_state_.mouse_moved_since_pressed && !io.WantCaptureMouse) {
        if (io.MousePos.x > 0 && io.MousePos.y > 0) {
            ops.should_process_click_select = true;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        selection_state_.mouse_moved_since_pressed = false;
    }
}

void ViewerApp::ProcessDelayedOps(FrameInternalDelayedOps& ops) {

    auto & rhi = RHI::Get();

    if (ops.should_start_baking) {
        ops.should_start_baking = false;
        baking_state_.is_baking_mode = true;
        baking_state_.baking_frame_index = 0;
        baking_state_.baking_camera_index = 0;
        auto dir = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/camera_configs");
        baking_state_.baking_camera_positions.clear();
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".json") {
                std::ifstream i(entry.path());
                nlohmann::json j;
                i >> j;
                Camera cam;
                cam.position = glm::vec3(j["camera_position"][0], j["camera_position"][1], j["camera_position"][2]);
                cam.direction = glm::vec3(j["camera_direction"][0], j["camera_direction"][1], j["camera_direction"][2]);
                cam.fov_Y = j["camera_fov_Y"];
                cam.near_plane = j["camera_near_plane"];
                cam.far_plane = j["camera_far_plane"];
                cam.up = glm::vec3(j["camera_up"][0], j["camera_up"][1], j["camera_up"][2]);
                baking_state_.baking_camera_positions.push_back(cam);
            }
        }
        MI_LOG(MIInfraLogType::kInfo, "Loaded {} baking camera positions.", baking_state_.baking_camera_positions.size());
        if (!baking_state_.baking_camera_positions.empty()) {
            view_->camera_ = baking_state_.baking_camera_positions[0];
        }
    }

    auto& queue = RHI::Get().GetGraphicsCommandQueue();
    auto ExportTexRaw = [&](const std::filesystem::path& dir, RDGTexture* rdgTex, const char* name, size_t bpp, std::string fmt) {
        if (!rdgTex) return;
        // Use the generic helper to read bytes
        std::vector<std::byte> bytes;
        uint32_t w = 0, h = 0;
        // Try to deduce PixelFormatType from fmt string for bpp; if not recognized, fallback using bpp
        PixelFormatType pf = PixelFormatType::kUnknown;
        // Simple mapping for common cases used below
        if (fmt == "r32f") pf = PixelFormatType::kR32_FLOAT;
        else if (fmt == "r8") pf = PixelFormatType::kR8_UNORM;
        else if (fmt == "rgba8") pf = PixelFormatType::kR8G8B8A8_UNORM;
        else if (fmt == "rgba16f") pf = PixelFormatType::kR16G16B16A16_FLOAT;
        else if (fmt == "r32u") pf = PixelFormatType::kR32_UINT;
        // If still unknown, approximate by channel count from bpp (assume 4 channels of 1 byte when bpp==4, etc.)
        if (pf == PixelFormatType::kUnknown) {
            if (bpp == 4) pf = PixelFormatType::kR8G8B8A8_UNORM;
            else if (bpp == 1) pf = PixelFormatType::kR8_UNORM;
            else if (bpp == 2) pf = PixelFormatType::kR16G16_FLOAT;
            else if (bpp == 8) pf = PixelFormatType::kR16G16B16A16_FLOAT;
            else if (bpp == 16) pf = PixelFormatType::kR32G32B32A32_FLOAT;
        }
        if (!ReadbackRDGTextureToBytes(rdgTex, pf, bytes, w, h)) return;

        auto bin_path = dir / std::format("{}_{}x{}_{}bpp.bin", name, w, h, bpp);
        std::ofstream ofs(bin_path, std::ios::binary);
        if (ofs && !bytes.empty()) {
            ofs.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        auto meta_path = dir / std::format("{}_meta.txt", name);
        std::ofstream meta(meta_path, std::ios::out);
        if (meta) {
            meta << std::format("name={}, width={}, height={}, bytes_per_pixel={}, fmt={}\n",
                name, w, h, bpp, fmt);
        }
    };
    if (ops.should_export_result) {
        ops.should_export_result = false;

        auto now = std::chrono::system_clock::now();
        auto t_c = std::chrono::system_clock::to_time_t(now);
        std::tm tm_local{};
        localtime_s(&tm_local, &t_c);
        std::string stamp = std::format("{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}",
            tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
            tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);

        auto data_dir = std::filesystem::path(R"(F:\\CLionProjects\\naf_pre\\data\\volprims)");

        std::filesystem::path out_dir = data_dir / std::format("f{}_{}", rhi.GetFrameIndex(), stamp);
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);

        ExportTexRaw(out_dir, view_->g_buffer_->G_depth_.Raw(), "depth", sizeof(float), "r32f");
        ExportTexRaw(out_dir, view_->g_buffer_->G_transmittance_.Raw(), "transmittance", sizeof(uint8_t), "r8");
        ExportTexRaw(out_dir, view_->volume_primitives_->volume_sample_color_.Raw(), "vol_sample_color", sizeof(uint8_t) * 4, "rgba8");
        ExportTexRaw(out_dir, view_->volume_primitives_->volume_sample_linear_depth_.Raw(), "vol_sample_linear_depth", sizeof(float), "r32f");
        ExportTexRaw(out_dir, view_->volume_primitives_->G_volume_density_.Raw(), "vol_density", sizeof(float), "r32f");
        ExportTexRaw(out_dir, view_->volume_primitives_->G_volume_color_.Raw(), "vol_color", sizeof(uint8_t) * 4, "rgba8");
        ExportTexRaw(out_dir, view_->volume_direct_lighting_->radiance.Raw(), "vol_direct_lighting", sizeof(uint16_t) * 4, "rgba16f");
        ExportTexRaw(out_dir, view_->volume_indirect_lighting_->radiance.Raw(), "vol_indirect_lighting", sizeof(uint16_t) * 4, "rgba16f");

        MI_LOG(MIInfraLogType::kInfo, "Frame data exported to {}", out_dir.string());
        ops.should_export_result = false;
    }

    if (ops.should_export_baking_result) {
        ops.should_export_baking_result = false;
        auto dir = GetInfra().TranslateResPathToFilePath("baking_results/");
        if (!std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
        auto date_time = GetCurrentDateTimeString();
        auto random_str = GenerateRandomString(6);
        auto out_dir = dir / (date_time + random_str);
        std::filesystem::create_directories(out_dir);

        ExportTexRaw(out_dir, view_->persistent_data_->path_tracing_film_.Raw(), "baked_radiance", sizeof(uint16_t) * 4, "rgba32f");

        if (baking_state_.baking_camera_index <= baking_state_.baking_camera_positions.size()) {
            auto& cam = baking_state_.baking_camera_positions[baking_state_.baking_camera_index];
            view_->camera_ = cam;
            MI_LOG(MIInfraLogType::kInfo, "Switched to baking camera index {}.", baking_state_.baking_camera_index);
        }
    }
    auto & io = ImGui::GetIO();
    if (ops.should_process_click_select) {
        ops.should_process_click_select = false;
        float mouse_x = io.MousePos.x;
        float mouse_y = io.MousePos.y;
        auto rhi_visibility = view_->g_buffer_->G_visibility_->GetRHI();
        auto rhi_fwd_depth = view_->forward_depth_->GetRHI();
        auto readback_buffer_visibility = rhi.CreateBuffer(
            rhi_visibility->GetWidth() * rhi_visibility->GetHeight() * sizeof(uint32_t) * 4,
            RHIBufferUsageFlagBits::kReadback
        );
        auto readback_buffer_depth = rhi.CreateBuffer(
            rhi_fwd_depth->GetWidth() * rhi_fwd_depth->GetHeight() * sizeof(float),
            RHIBufferUsageFlagBits::kReadback
        );
        queue.MemoryBarrier();
        queue.TextureBarrier(rhi_visibility, RHITextureLayoutType::kTransferSrcOptimal,
            RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
            RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
        queue.TextureBarrier(rhi_fwd_depth, RHITextureLayoutType::kTransferSrcOptimal,
            RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
            RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
        queue.CopyTextureToBuffer(rhi_visibility, readback_buffer_visibility.Raw());
        queue.CopyTextureToBuffer(rhi_fwd_depth, readback_buffer_depth.Raw());
        view_->g_buffer_->G_visibility_->Use(
            RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kTransferRead,
            RHITextureLayoutType::kTransferSrcOptimal
        );
        view_->forward_depth_->Use(
            RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kTransferRead,
            RHITextureLayoutType::kTransferSrcOptimal
        );
        queue.MemoryBarrier();
        queue.WaitForIdle("Readback Buffers");
        auto ptr = (glm::uvec4*)readback_buffer_visibility->Map();
        glm::uvec4 pixel = ptr[(int(mouse_y) * rhi_visibility->GetWidth() + int(mouse_x))];
        auto depth_ptr = (float*)readback_buffer_depth->Map();
        input_state_.last_click_forward_depth_ = depth_ptr[(int(mouse_y) * rhi_fwd_depth->GetWidth() + int(mouse_x))];
        readback_buffer_visibility->Unmap();
        readback_buffer_depth->Unmap();
        float uv_x = std::bit_cast<float>(pixel.z);
        float uv_y = std::bit_cast<float>(pixel.w);
        auto descriptor_rank = pixel.x >> 24;
        auto renderable_index = pixel.x & 0xFFFFFF;
        if (renderable_index == 0xFFFFFF) {
            renderable_index = UINT32_MAX;
        }
        auto primitive_index = pixel.y;
        glm::vec2 uv = {uv_x, uv_y};
        if (selection_state_.selected_renderable_index != renderable_index) {
            if (renderable_index == UINT32_MAX) {
                arrow_mesh_x_instance_->SetVisible(false);
                arrow_mesh_y_instance_->SetVisible(false);
                arrow_mesh_z_instance_->SetVisible(false);
                selection_state_.selected_deferred_renderable_index = UINT32_MAX;
            } else if (renderable_index != arrow_mesh_x_instance_->GetIndex()
                && renderable_index != arrow_mesh_y_instance_->GetIndex()
                && renderable_index != arrow_mesh_z_instance_->GetIndex()
            ) {
                selection_state_.selected_deferred_renderable_index = renderable_index;
                auto renderable = scene_->GetRenderables()[renderable_index].Raw();
                arrow_mesh_x_instance_->EditTransform().position = renderable->GetTransform().position;
                arrow_mesh_x_instance_->SetVisible(true);
                arrow_mesh_y_instance_->EditTransform().position = renderable->GetTransform().position;
                arrow_mesh_y_instance_->SetVisible(true);
                arrow_mesh_z_instance_->EditTransform().position = renderable->GetTransform().position;
                arrow_mesh_z_instance_->SetVisible(true);
            }
        }
        selection_state_.selected_descriptor_rank = descriptor_rank;
        selection_state_.selected_renderable_index = renderable_index;
        selection_state_.selected_primitive_index = primitive_index;
        selection_state_.selected_uv = uv;
        MI_LOG(MIInfraLogType::kInfo, "Selected Renderable {}, Primitive {}, Descriptor Rank {}, UV ({}, {})",
            selection_state_.selected_renderable_index, selection_state_.selected_primitive_index, selection_state_.selected_descriptor_rank, selection_state_.selected_uv.x, selection_state_.selected_uv.y
        );
    }

    if (ops.should_reload_shaders) {
        RHI::Get().WaitForIdle();
        RDGShaderLibrary::Get().RecompileUpdatedCachedShaders();
        ops.should_reload_shaders = false;
    }
}

void ViewerApp::ProcessAxisDragging() {
    auto io = ImGui::GetIO();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.WantCaptureMouse) {
        int axis = -1;
        if (selection_state_.selected_renderable_index == arrow_mesh_x_instance_->GetIndex()) axis = 0;
        else if (selection_state_.selected_renderable_index == arrow_mesh_y_instance_->GetIndex()) axis = 1;
        else if (selection_state_.selected_renderable_index == arrow_mesh_z_instance_->GetIndex()) axis = 2;
        if (axis != -1) {
            glm::vec2 mouse;
            mouse.x = io.MousePos.x;
            mouse.y = io.MousePos.y;
            if (!input_state_.dragging_ && !io.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                // 在箭头上左键点下，此时开始拖拽
                input_state_.drag_mouse_start_pos_ = mouse;
                if (selection_state_.selected_deferred_renderable_index != UINT32_MAX) {
                    auto renderable = scene_->GetRenderables()[selection_state_.selected_deferred_renderable_index].Raw();
                    input_state_.drag_start_obj_pos_ = renderable->GetTransform().position;
                }
                input_state_.dragging_ = true;
            }
            if (input_state_.dragging_) {
                glm::vec3 end_world_pos {};
                auto& camera = view_->camera_;
                glm::vec2 ndc2 = {
                    (input_state_.drag_mouse_start_pos_.x / (float)view_->film_width_) * 2.0f - 1.0f,
                    1.0f - (input_state_.drag_mouse_start_pos_.y / (float)view_->film_height_) * 2.0f
                };
                float linear_depth = camera.ReversedZDepthToLinearDepth(input_state_.last_click_forward_depth_);
                float aspect = float(view_->film_width_) / float(view_->film_height_);
                glm::vec3 start_world_pos = camera.RecoverWorldPositionNDC2(ndc2, linear_depth, aspect);
                glm::vec2 curr_ndc2 = {
                    (mouse.x / (float)view_->film_width_) * 2.0f - 1.0f,
                    1.0f - (mouse.y / (float)view_->film_height_) * 2.0f
                };
                glm::vec3 cursor_end_world_pos = camera.RecoverWorldPositionNDC2(curr_ndc2, linear_depth, aspect);
                glm::vec3 delta = cursor_end_world_pos - start_world_pos;
                glm::vec3 delta_projected = {};
                delta_projected[axis] = delta[axis];
                end_world_pos = input_state_.drag_start_obj_pos_ + delta_projected;
                if (selection_state_.selected_deferred_renderable_index != UINT32_MAX) {
                    auto renderable = scene_->GetRenderables()[selection_state_.selected_deferred_renderable_index].Raw();
                    renderable->EditTransform().position = end_world_pos;
                    arrow_mesh_x_instance_->EditTransform().position = end_world_pos;
                    arrow_mesh_y_instance_->EditTransform().position = end_world_pos;
                    arrow_mesh_z_instance_->EditTransform().position = end_world_pos;
                }
            }
        } else {
            input_state_.dragging_ = false;
        }
    } else {
        input_state_.dragging_ = false;
    }
}

void ViewerApp::Run(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg) {

    auto & rhi = RHI::Get();

    float cpu_duration = 0.0f;
    std::vector<RDGTimePeriod> rdg_time_periods;
    RDGProfilingContextRef pending_profiling_context;

    std::future<void> previous_frame_future;
    TRef<RHISyncPoint> previous_frame_sync_point = rhi.CreateSyncPoint();

    bool first_frame = true;

    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        auto cpu_tp_start = std::chrono::steady_clock::now();

        FrameInternalDelayedOps ops {};

        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        HandleNavigationInput(cpu_duration);
        HandleKeyboardShortcuts(ops);

        // Combined UI: Console (left) + Rendering/Performance (right)
        {
            ImGui::SetNextWindowSize(ImVec2(1200, 700), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("UI", nullptr, ImGuiWindowFlags_NoCollapse)) {
                float full_w = ImGui::GetContentRegionAvail().x;
                float full_h = ImGui::GetContentRegionAvail().y;
                float spacing = ImGui::GetStyle().ItemSpacing.x;

                // Right column: max 550 by default.
                float right_w = std::min(550.0f, full_w * 0.7f);
                float left_w = std::max(350.0f, full_w - right_w - spacing);

                ImGui::BeginChild("UI_Left", ImVec2(left_w, 0), true);
                auto content_region = ImGui::GetContentRegionAvail();
                console_.DrawImGuiConsoleEmbedded({content_region.x, content_region.y});
                ImGui::EndChild();

                ImGui::SameLine(0.0f, spacing);

                ImGui::BeginChild("UI_Right", ImVec2(right_w, 0), true);
                HandleControlUILogic(ops, rdg_time_periods, cpu_duration);
                ImGui::EndChild();
            }
            ImGui::End();
        }

        // console_.DrawImGuiConsole(); // replaced by embedded console in combined UI window

        if (baking_state_.is_baking_mode) {
            baking_state_.baking_frame_index++;
            if (baking_state_.baking_frame_index == baking_state_.baking_max_num_frames) {
                baking_state_.baking_camera_index++;
                baking_state_.baking_frame_index = 0;
                ops.should_export_baking_result = true;
            }
            if (baking_state_.baking_camera_index >= baking_state_.baking_camera_positions.size()) {
                baking_state_.ClearBakingState();
                ops.should_export_baking_result = false;
                MI_LOG(MIInfraLogType::kInfo, "Baking complete.");
            }
        }

        ProcessClickSelect(ops);

        // Process pending ZMQ messages
        auto frame_export_requests = zmq_server_->PollEvents();
        bool any_export_requests_pending = false;

        // RDG Execution!
        auto & io = ImGui::GetIO();
        RDGProfilingContextRef current_profiling_context;
        {
            RenderGraphBuilder builder;
            bool should_render_scene = !suspended_ || one_frame_rendering_requested_;
            RenderFrame(builder, view_.get(), should_render_scene);
            // Clear flag
            one_frame_rendering_requested_ = false;

            // Mark export flags.
            if (ops.should_process_click_select && !io.WantCaptureMouse) {
                view_->forward_depth_->SetExport();
                view_->g_buffer_->G_visibility_->SetExport();
            }

            if (ops.should_export_result) {
                view_->g_buffer_->G_depth_->SetExport();
                view_->g_buffer_->G_transmittance_->SetExport();
                view_->volume_primitives_->volume_sample_color_->SetExport();
                view_->volume_primitives_->volume_sample_linear_depth_->SetExport();
                view_->volume_primitives_->G_volume_density_->SetExport();
                view_->volume_primitives_->G_volume_color_->SetExport();
                view_->volume_direct_lighting_->radiance->SetExport();
                view_->volume_indirect_lighting_->radiance->SetExport();
            }

            if (ops.should_export_baking_result) {
                view_->radiance_->SetExport();
            }

            for (auto e : frame_export_requests) {
                if (e == "radiance") {
                    any_export_requests_pending = true;
                    view_->radiance_->SetExport();
                }
                if (e == "overlay") {
                    any_export_requests_pending = true;
                    view_->overlay_->SetExport();
                }
                if (e == "depth") {
                    any_export_requests_pending = true;
                    view_->g_buffer_->G_depth_->SetExport();
                }
                if (e == "grf_depth") {
                    any_export_requests_pending = true;
                    view_->grf_->stochastic_rendering_depth_->SetExport();
                }
                // TODO more types...
            }

            std::string frame_name = "Frame " + std::to_string(GetFrameIndexForCurrentThread());
            auto graph = builder.Compile(frame_name);
            {
                graph->Execute(pool_.Raw());
            }

            current_profiling_context = graph->GetProfilingContext();
        }

        if (any_export_requests_pending) {
            // Just wait for this frame to finish and process requests.
            RHI::Get().WaitForIdle();
            // Export!
            if (!exported_render_results_.empty()) {
                MI_WARN("Seems that some exported render results are not consumed...");
                exported_render_results_.clear();
            }

            auto PackOne = [&](const std::string& type, RDGTexture* tex, PixelFormatType fmt) {
                if (!tex) return;
                std::vector<std::byte> bytes;
                uint32_t w = 0, h = 0;
                if (ReadbackRDGTextureToBytes(tex, fmt, bytes, w, h)) {
                    ExportedRenderResult res;
                    res.bytes = std::move(bytes);
                    res.width = w;
                    res.height = h;
                    res.format = fmt;
                    res.name = type;
                    exported_render_results_.push_back(std::move(res));
                } else {
                    MI_WARN("Failed to read back '{}' texture", type.c_str());
                }
            };

            for (const auto &e : frame_export_requests) {
                if (e == "radiance") {
                    PackOne(e, view_->radiance_.Raw(), PixelFormatType::kR16G16B16A16_FLOAT);
                } else if (e == "overlay") {
                    PackOne(e, view_->overlay_.Raw(), PixelFormatType::kR8G8B8A8_UNORM);
                } else if (e == "depth") {
                    PackOne(e, view_->g_buffer_->G_depth_.Raw(), PixelFormatType::kD32_FLOAT);
                } else if (e == "grf_depth") {
                    PackOne(e, view_->grf_->stochastic_rendering_depth_.Raw(), PixelFormatType::kD32_FLOAT);
                } else if (e == "transmittance") {
                    PackOne(e, view_->g_buffer_->G_transmittance_.Raw(), PixelFormatType::kR8_UNORM);
                } else if (e == "visibility") {
                    PackOne(e, view_->g_buffer_->G_visibility_.Raw(), PixelFormatType::kR32G32B32A32_UINT);
                } else {
                    MI_WARN("Unknown export type '{}', skipping", e.c_str());
                }
            }
            // Reply to the waiting ZMQ client with the exported results.
            if (zmq_server_) {
                zmq_server_->ReplyExportedFrame();
            }
        }


        ProcessDelayedOps(ops);

        // 必须在 selection 更新后判定拖拽
        ProcessAxisDragging();

        if (rhi.GetFrameIndex() % 1000 == 0) {
            printf("[%llu] Pool memory: %.2f MB\n", rhi.GetFrameIndex(), pool_->GetTotalDeviceMemoryUsage() / 1024.0f / 1024.0f);
#ifndef NDEBUG
            printf("RefCounted object count: %u\n", GetRefCountedObjectCount());
            printf("RHI object count: %llu\n", RHIResource::GetLivingRHIResourceCount());
#endif
            printf("Device allocator allocated memory: %.2f MB\n", resource_allocator_->GetTotalAllocatedDeviceSize() / 1024.0f / 1024.0f);
            fflush(stdout);
        }

        if (first_frame) {
            first_frame = false;
        } else {
            if (previous_frame_future.valid()) {
                previous_frame_future.wait();
            }
            previous_frame_sync_point->Wait();
            previous_frame_sync_point->Reset();
        }

        // Time to recycle renderables and rendering resources marked for deletion from the previous frame!
        scene_->FlushRemovingRenderables();

        // Try resolve previous frame profiling results (non-blocking) after the previous frame has finished.
        // In case if not ready yet, keep last results.
        if (pending_profiling_context) {
            std::vector<RDGTimePeriod> resolved;
            if (pending_profiling_context->ResolveTimestampPeriods(resolved, rhi, RHI::RHITimestampQueryMode::kNonBlocking)) {
                rdg_time_periods = std::move(resolved);
            }
            pending_profiling_context.SafeRelease();
        }
        // Set current profiling context as pending for next frame.
        pending_profiling_context = current_profiling_context;

        previous_frame_future = rhi.AdvanceFrame(previous_frame_sync_point.Raw());
        fflush(stdout);
        auto cpu_tp_end = std::chrono::steady_clock::now();
        cpu_duration = std::chrono::duration<float>(cpu_tp_end - cpu_tp_start).count();
    }
}

std::vector<ViewerApp::ExportedRenderResult> ViewerApp::GetAndClearExportedFrameResults() {
    return std::move(exported_render_results_);
}

ViewerApp::ViewerStatus ViewerApp::GetStatus() {
    ViewerStatus s{};
    s.is_suspended = suspended_.load();
    auto &rhi = RHI::Get();
    s.frame_index = (uint32_t)rhi.GetFrameIndex();
    if (view_) {
        s.camera = view_->camera_;
    }
    return s;
}

bool ViewerApp::LoadGLTFAbsolute(const std::filesystem::path& path, std::vector<uint32_t>* out_renderable_indices) {
    if (!scene_ || !resource_allocator_) return false;
    if (path.empty() || !std::filesystem::exists(path)) {
        MI_WARN("LoadGLTFAbsolute: file not found '{}'.", path.string());
        return false;
    }

    std::vector<TRef<Geometry>> geometries;
    std::vector<TRef<Material>> materials;
    std::vector<TRef<RenderableNode>> nodes;
    std::vector<TRef<StaticMeshInstance>> new_meshes;
    if (!GLTFLoader::LoadGLTF(
        path,
        *resource_allocator_,
        *scene_,
        default_material_.Raw(),
        geometries,
        materials,
        new_meshes,
        &nodes)) {
        MI_WARN("LoadGLTFAbsolute: failed to load GLTF '{}'.", path.string());
        return false;
    }

    RegisterLoadedScene(path.filename().string(), nodes);
    auto & r = Renderer::Get();
    auto & rhi = RHI::Get();
    for (auto e : new_meshes) {
        if (e) {
            e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
            if (out_renderable_indices) out_renderable_indices->push_back(e->GetIndex());
        }
    }
    return true;
}

bool ViewerApp::LoadPLYAsGRFAbsolute(const std::filesystem::path& path, std::vector<uint32_t>& out_renderable_indices) {
    if (!scene_ || !resource_allocator_) return false;
    if (path.empty() || !std::filesystem::exists(path)) {
        MI_WARN("LoadPLYAbsolute: file not found '{}'.", path.string());
        return false;
    }

    TRef<GaussianRadianceField> grf;
    if (!GaussianRadianceFieldLoader::LoadPLY(path, *resource_allocator_, grf)) {
        MI_WARN("LoadPLYAbsolute: failed to load PLY '{}'.", path.string());
        return false;
    }
    grf->UpdateOnDevice(resource_allocator_.Raw());
    auto inst = GaussianRadianceFieldInstance::Create(scene_.get(), grf.Raw(), Transform::FromMatrix(glm::mat4(1.0f)));
    if (inst) {
        out_renderable_indices.push_back(inst->GetIndex());
        auto node = renderable_node_registry_->Create(scene_.get(), path.filename().string());
        node->SetRenderable(inst.Raw());
        node->UpdateWorldTransform();
        RegisterLoadedScene(path.filename().string(), { node });
    }
    return inst.IsValid();
}

bool ViewerApp::RemoveRenderableNodeByIndex(uint32_t renderable_node_index) {
    if (!scene_) return false;
    mi_assert(false, "Not implemented yet.");
    return true;
}

bool ViewerApp::CleanAllRenderableNodes() {
    if (!scene_) return false;
    loaded_scenes_.clear();

    selection_state_ = {};
    if (arrow_mesh_x_instance_) arrow_mesh_x_instance_->SetVisible(false);
    if (arrow_mesh_y_instance_) arrow_mesh_y_instance_->SetVisible(false);
    if (arrow_mesh_z_instance_) arrow_mesh_z_instance_->SetVisible(false);
    return true;
}

void Run3DViewer(std::unique_ptr<MIInfraInterface> &&infra, const MainLoopStartConfig &cfg) {
    ViewerApp app;

    app.Initialize(std::move(infra), cfg);
    app.LoadScene(cfg);

    app.Run(std::move(infra), cfg);

    RHI::Get().WaitForIdle();
    app.Destroy();
}

MI_NAMESPACE_END

