#include "viewer_app.h"

#include <fstream>
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

#include "../../renderer/renderer/r_volume_direct_lighting.h"
#include "../../renderer/renderer/r_volume_indirect_lighting.h"
#include "../../renderer/renderer/r_volume_primitives.h"
#include "../../renderer/renderer/r_persistent.h"

#include "rdg/rdg_shader.h"
#include "util/texture_loader.h"
#include "util/gltf_loader.h"
#include "util/volprims_loader.h"
#include "util/gaussian_radiance_field_loader.h"

#include "3d_viewer.h"

MI_NAMESPACE_BEGIN

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
    std::string str(len, 0);
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

void ViewerApp::Initialize(std::unique_ptr<MIInfraInterface>&& infra, const MainLoopStartConfig& cfg) {
    auto pwd = std::filesystem::current_path();

    TransferInfra(std::move(infra));
    GetInfra().Init();
    console_.Initialize();

    if (GetCurrentThreadType() != ThreadType::kUnknown) {
        mi_assert(false, "MainLoop: somehow the thread calling Start() is known.");
    }
    SetCurrentThreadType(ThreadType::kRenderThread);

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
}

void ViewerApp::Destroy() {

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

    meshes_.clear();

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

    console_.Destroy();

    GetInfra().Shutdown();
    DestroyInfra();

    glfwTerminate();
}

void ViewerApp::LoadScene(const MainLoopStartConfig& cfg) {

    scene_ = std::make_unique<Scene>();

    if (true) {
        sky_cube_ = TextureLoader::LoadEnvironmentMap("SkyTexture",
            GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/tief_etz_4k.exr"));
        sky_cube_->UpdateOnDevice();
        sky_cube_->ConvertToBindless();
    }
    default_material_ = Material::Create("default_mat", {0.8f, 0.8f, 0.8f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});

    {
        auto arrow_mat_x = Material::Create("arrow_mat_x", {1.0f, 0.0f, 0.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        auto arrow_mat_y = Material::Create("arrow_mat_y", {0.0f, 1.0f, 0.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        auto arrow_mat_z = Material::Create("arrow_mat_z", {0.0f, 0.0f, 1.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        arrow_mat_x->SetForward(true);
        arrow_mat_y->SetForward(true);
        arrow_mat_z->SetForward(true);
        auto& r = Renderer::Get();
        arrow_mat_x->UpdateOnDevice(r.GetDeviceAllocator());
        arrow_mat_y->UpdateOnDevice(r.GetDeviceAllocator());
        arrow_mat_z->UpdateOnDevice(r.GetDeviceAllocator());
        TRef<StaticMeshInstance> original_arrow_instance;
        {
            std::vector<TRef<Geometry>> geometries;
            std::vector<TRef<Material>> materials;
            auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/internal/arrow.gltf");
            if (!GLTFLoader::LoadGLTF(
                model_path,
                *resource_allocator_,
                *scene_, default_material_.Raw(),
                geometries, materials, meshes_
            )) {
                MI_WARN("Failed to load GLTF model {}.", model_path.string());
            }
            original_arrow_instance = meshes_.back();
            auto mesh = original_arrow_instance->GetStaticMesh();
            arrow_geometry_ = mesh->GetGeometries()[0];
        }
        {
            float scale = 0.25f;
            arrow_mesh_x_ = StaticMesh::Create(false, false);
            arrow_mesh_x_->AddMeshPrimitive(arrow_geometry_.Raw(), arrow_mat_x.Raw());
            arrow_mesh_x_->UpdateOnDevice(r.GetDeviceAllocator());
            arrow_mesh_x_instance_ = StaticMeshInstance::Create(scene_.get(), arrow_mesh_x_.Raw(),
                original_arrow_instance->GetTransform().Scaled(glm::vec3(scale)));

            arrow_mesh_y_ = StaticMesh::Create(false, false);
            arrow_mesh_y_->AddMeshPrimitive(arrow_geometry_.Raw(), arrow_mat_y.Raw());
            arrow_mesh_y_->UpdateOnDevice(r.GetDeviceAllocator());
            arrow_mesh_y_instance_ = StaticMeshInstance::Create(scene_.get(), arrow_mesh_y_.Raw(),
                original_arrow_instance->GetTransform().RotatedAbout(glm::radians(90.0f), {0, 0, 1}).Scaled(glm::vec3(scale)));

            arrow_mesh_z_ = StaticMesh::Create(false, false);
            arrow_mesh_z_->AddMeshPrimitive(arrow_geometry_.Raw(), arrow_mat_z.Raw());
            arrow_mesh_z_->UpdateOnDevice(r.GetDeviceAllocator());
            arrow_mesh_z_instance_ = StaticMeshInstance::Create(scene_.get(), arrow_mesh_z_.Raw(),
                original_arrow_instance->GetTransform().RotatedAbout(glm::radians(-90.0f), {0, 1, 0}).Scaled(glm::vec3(scale)));
        }
        scene_->RemoveRenderable(original_arrow_instance.Raw());
        arrow_mesh_x_instance_->SetVisible(false);
        arrow_mesh_y_instance_->SetVisible(false);
        arrow_mesh_z_instance_->SetVisible(false);
    }

    if (false) {
        std::vector<TRef<Geometry>> geometries;
        std::vector<TRef<Material>> materials;
        auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/box/scene.gltf");
        if (!GLTFLoader::LoadGLTF(
            model_path,
            *resource_allocator_,
            *scene_, default_material_.Raw(),
            geometries, materials, meshes_
        )) {
            MI_WARN("Failed to load GLTF model {}.", model_path.string());
        }
    }
    if (true) {
        TRef<GaussianRadianceField> field;
        if (!GaussianRadianceFieldLoader::LoadPLY("F:/CLionProjects/3DGS_GI/data/counter/point_cloud/iteration_30000/point_cloud.ply",
            *resource_allocator_, field)) {
            MI_WARN("Failed to load Gaussian Radiance Field PLY.");
        }
        if (field) {
            field->UpdateOnDevice(resource_allocator_.Raw());
            auto inst = GaussianRadianceFieldInstance::Create(scene_.get(), field.Raw());
        }
    }

    scene_->SetSkyCube(sky_cube_.Raw());
    scene_->CreateOnDevice();

    view_ = std::make_unique<RendererView>();
    view_->film_width_ = cfg.window_width;
    view_->film_height_ = cfg.window_height;
    view_->scene_ = scene_.get();

    scene_->directional_light_.direction = glm::normalize(glm::vec3(-5.5f, -4.4f, 5.5f));
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

void ViewerApp::HandleUILogic(FrameInternalDelayedOps& ops, std::vector<RDGTimePeriod> time_periods, float cpu_duration) {

    auto& io = ImGui::GetIO();
    CVar_DebugCursorScreenCoordsX.Set((int)round(io.MousePos.x));
    CVar_DebugCursorScreenCoordsY.Set((int)round(io.MousePos.y));

    {
        ImGui::Begin("Rendering");
        if (ImGui::Button("Reload Shaders")) {
            ops.should_reload_shaders = true;
        }
        if (ImGui::CollapsingHeader("Selected Renderable")) {
            if (selection_state_.selected_deferred_renderable_index != UINT32_MAX) {
                auto renderable = scene_->GetRenderables()[selection_state_.selected_deferred_renderable_index];
                ImGui::Text("Index: %d", renderable->GetIndex());
                ImGui::Text("Type: %s", ToString(renderable->GetType()).c_str());
                Transform& t = renderable->EditTransform();
                ImGui::InputFloat3("Position", &t.position[0]);
                ImGui::InputFloat3("Rotation", &t.rotation[0]);
                ImGui::InputFloat3("Scale", &t.scale[0]);
                ImGui::Text("AABB: Min(%.2f, %.2f, %.2f) Max(%.2f, %.2f, %.2f)",
                    renderable->GetAABB().min.x, renderable->GetAABB().min.y, renderable->GetAABB().min.z,
                    renderable->GetAABB().max.x, renderable->GetAABB().max.y, renderable->GetAABB().max.z
                );
                bool hide = renderable->IsVisible();
                ImGui::Checkbox("Visible", &hide);
                renderable->SetVisible(hide);
            } else {
                ImGui::Text("None");
                if (ImGui::Button("Reveal All Hidden")) {
                    for (const auto& r : scene_->GetRenderables()) {
                        if (r) r->SetVisible(true);
                    }
                }
            }
        }
        if (!baking_state_.is_baking_mode) {
            if (ImGui::Button("Start Baking")) {
                ops.should_start_baking = true;
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Halt Baking")) {
                baking_state_.is_baking_mode = false;
            }
            ImGui::PopStyleColor();
        }
        if (baking_state_.is_baking_mode) {
            float fraction = (float)baking_state_.baking_frame_index / (float)baking_state_.baking_max_num_frames;
            ImGui::Text("Baking Mode: Camera %d / %d, Frame %d / %d",
                baking_state_.baking_camera_index + 1, (uint32_t)baking_state_.baking_camera_positions.size(),
                baking_state_.baking_frame_index + 1, baking_state_.baking_max_num_frames
            );
            float remaining_frames = (float)(((int)baking_state_.baking_camera_positions.size() - baking_state_.baking_camera_index - 1) * baking_state_.baking_max_num_frames
                + (baking_state_.baking_max_num_frames - baking_state_.baking_frame_index));
            float avg_frame_time = cpu_duration;
            float raw_remaining_time_sec = remaining_frames * avg_frame_time;
            static float remaining_time_sec = 0;
            remaining_time_sec = remaining_time_sec * 0.99f + raw_remaining_time_sec * 0.01f;
            float remaining_time_min = remaining_time_sec / 60.0f;
            float remaining_time_hr = remaining_time_min / 60.0f;
            float rest_remaining_time_min = fmod(remaining_time_min, 60.0f);
            float rest_remaining_time_sec = fmod(remaining_time_sec, 60.0f);
            int show_hr = (int)floor(remaining_time_hr);
            int show_min = (int)floor(rest_remaining_time_min);
            int show_sec = (int)floor(rest_remaining_time_sec);
            ImGui::Text("ETA: %02d:%02d:%02d", show_hr, show_min, show_sec);
            ImGui::SameLine();
            ImGui::ProgressBar(fraction, ImVec2(0.0f, 0.0f));
        }
        if (ImGui::CollapsingHeader("CVars")) {
            auto& cvar_registry = CVarRegistry::GetInstance();
            auto cvar_list = cvar_registry.GetAllCVars();
            std::sort(cvar_list.begin(), cvar_list.end(),
                [](CVarBase* a, CVarBase* b) {
                    return a->GetId() < b->GetId();
                }
            );
            std::function<void(uint32_t,uint32_t, std::string, uint32_t)> DisplayCVars
                = [&](uint32_t begin, uint32_t end, std::string category, uint32_t prefix_length) {
                    if (begin >= end) return;
                    while (begin < end) {
                        auto id = cvar_list[begin]->GetId();
                        if (id.length() >= prefix_length) {
                            break;
                        }
                        auto e = cvar_list[begin];
                        auto cvar_name = category;
                        if (e->GetType() == CVarType::kBool) {
                            auto cvar = static_cast<CVar<bool>*>(e);
                            bool value = cvar->Get();
                            if (ImGui::Checkbox(cvar_name.c_str(), &value)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kFloat) {
                            auto cvar = static_cast<CVar<float>*>(e);
                            float value = cvar->Get();
                            if (ImGui::DragFloat(cvar_name.c_str(), &value, 0.01f)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kFloat2) {
                            auto cvar = static_cast<CVar<glm::vec2>*>(e);
                            glm::vec2 value = cvar->Get();
                            if (ImGui::DragFloat2(cvar_name.c_str(), &value[0], 0.01f)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kFloat3) {
                            auto cvar = static_cast<CVar<glm::vec3>*>(e);
                            glm::vec3 value = cvar->Get();
                            if (ImGui::DragFloat3(cvar_name.c_str(), &value[0], 0.01f)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kFloat4) {
                            auto cvar = static_cast<CVar<glm::vec4>*>(e);
                            glm::vec4 value = cvar->Get();
                            if (ImGui::DragFloat4(cvar_name.c_str(), &value[0], 0.01f)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kInt) {
                            auto cvar = static_cast<CVar<int>*>(e);
                            int value = cvar->Get();
                            if (ImGui::DragInt(cvar_name.c_str(), &value)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kString) {
                            auto cvar = static_cast<CVar<std::string>*>(e);
                            std::string value = cvar->Get();
                            char buffer[256];
                            strncpy_s(buffer, value.c_str(), sizeof(buffer));
                            if (ImGui::InputText(cvar_name.c_str(), buffer, sizeof(buffer))) {
                                cvar->Set(std::string(buffer));
                            }
                        }
                        begin ++;
                    }
                    if (begin < end && ImGui::TreeNode(category.empty() ? "Root" : category.c_str())) {
                        for (uint32_t start = begin; start < end;) {
                            auto id = cvar_list[start]->GetId();
                            size_t next_delim = id.find('.', prefix_length);
                            std::string next_category;
                            if (next_delim != std::string::npos) {
                                next_category = id.substr(prefix_length, next_delim - prefix_length);
                            } else {
                                next_category = id.substr(prefix_length);
                            }
                            auto next_prefix_length = (uint32_t)(prefix_length + next_category.length() + 1);
                            size_t finish;
                            for (finish = start + 1; finish < end; finish++) {
                                auto next_id = cvar_list[finish]->GetId();
                                if (next_id.length() < next_prefix_length
                                || next_id.substr(0, next_prefix_length - 1) != id.substr(0, next_prefix_length - 1)) {
                                    DisplayCVars(start, (uint32_t)finish, next_category, next_prefix_length);
                                    start = (uint32_t)finish;
                                    break;
                                }
                            }
                            if (finish == end) {
                                DisplayCVars(start, end, next_category, next_prefix_length);
                                break;
                            }
                        }
                        ImGui::TreePop();
                    }
            };
            DisplayCVars(0, (uint32_t)cvar_list.size(), "", 0);
        }
        if (ImGui::CollapsingHeader("Performance")) {
            ImGui::Text("CPU: %.2f ms", cpu_duration * 1000.0);
            std::function<void(int, int, int)> DrawTree;
            DrawTree = [&](int start, int end, int depth) {
                ImGui::Indent(20);
                int last = start;
                for (int i = start; i < end; i++) {
                    if (time_periods[i].class_names.size() <= depth
                    ||  time_periods[i].class_names[depth] != time_periods[last].class_names[depth]) {
                        if (last != i) {
                            std::string node_name = time_periods[last].class_names[depth];
                            float duration = 0.0f;
                            for (int j = last; j < i; j++) {
                                duration += time_periods[j].duration;
                            }
                            std::string id = node_name;
                            node_name += std::format(" ({:.2f} ms)", duration * 1000);
                            if (ImGui::TreeNode(id.c_str(), "%s", node_name.c_str())) {
                                DrawTree(last, i, depth + 1);
                                ImGui::TreePop();
                            }
                        }
                        if (time_periods[i].class_names.size() <= depth) {
                            std::string node_name = time_periods[i].pass_name;
                            // Update stats per pass name from start of program.
                            auto & stat = perf_stats_[node_name];
                            float ms = time_periods[i].duration * 1000.0f;
                            stat.min_ms = std::min(stat.min_ms, ms);
                            stat.max_ms = std::max(stat.max_ms, ms);
                            ImGui::Text("%s: %.2f ms (min %.2f / max %.2f)", node_name.c_str(), ms, stat.min_ms, stat.max_ms);
                        }
                        last = i + 1;
                    }
                }
                if (last < end) {
                    std::string node_name = time_periods[last].class_names[depth];
                    float duration = 0.0f;
                    for (int j = last; j < end; j++) {
                        duration += time_periods[j].duration;
                    }
                    std::string id = node_name;
                    node_name += std::format(" ({:.2f} ms)", duration * 1000);
                    if (ImGui::TreeNode(id.c_str(), "%s", node_name.c_str())) {
                        DrawTree(last, end, depth + 1);
                        ImGui::TreePop();
                    }
                }
                ImGui::Unindent(20);
            };
            DrawTree(0, (int)time_periods.size(), 0);
        }
        ImGui::End();
    }
}

void ViewerApp::ExecConsoleCommand(std::string cmd) {
    // TODO: implement command execution
}

void ViewerApp::ProcessClickSelect(FrameInternalDelayedOps& ops) {
    auto & io = ImGui::GetIO();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !input_state_.dragging_) {
        ops.should_process_click_select = true;
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
    auto ExportTexRaw = [&](std::filesystem::path dir, auto rdgTex, const char* name, size_t bpp, std::string fmt) {
        if (!rdgTex) return;
        auto tex = rdgTex->GetRHI();
        if (!tex) return;
        const size_t w = tex->GetWidth();
        const size_t h = tex->GetHeight();
        const size_t byte_size = w * h * bpp;

        auto readback = rhi.CreateBuffer(byte_size, RHIBufferUsageFlagBits::kReadback);

        queue.MemoryBarrier();
        queue.TextureBarrier(tex, RHITextureLayoutType::kTransferSrcOptimal,
            RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
            RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
        queue.CopyTextureToBuffer(tex, readback.Raw());

        rdgTex->Use(RHIPipelineStageFlagBits::kTransfer,
            RHIGPUAccessFlagBits::kTransferRead, RHITextureLayoutType::kTransferSrcOptimal);

        queue.MemoryBarrier();
        queue.WaitForIdle("Export Frame");

        void* ptr = readback->Map();
        auto bin_path = dir / std::format("{}_{}x{}_{}bpp.bin", name, w, h, bpp);
        std::ofstream ofs(bin_path, std::ios::binary);
        if (ofs) ofs.write(reinterpret_cast<const char*>(ptr), byte_size);
        readback->Unmap();

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
            if (!input_state_.dragging_ && !io.WantCaptureMouse) {
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

    float cpu_duration = 0;
    std::vector<RDGTimePeriod> rdg_time_periods;

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
        HandleUILogic(ops, rdg_time_periods, cpu_duration);
        console_.DrawImGuiConsole();

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

        ProcessAxisDragging();


        auto & io = ImGui::GetIO();
        {
            RenderGraphBuilder builder;
            RenderFrame(builder, view_.get());

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

            std::string frame_name = "Frame " + std::to_string(GetFrameIndexForCurrentThread());
            auto graph = builder.Compile(frame_name);
            graph->Execute(pool_.Raw());

            rdg_time_periods = graph->GetTimestampPeriods();
        }


        ProcessDelayedOps(ops);


        if (rhi.GetFrameIndex() % 1000 == 0) {
            printf("[%llu] Pool memory: %.2f MB\n", rhi.GetFrameIndex(), pool_->GetTotalDeviceMemoryUsage() / 1024.0f / 1024.0f);
#ifndef NDEBUG
            printf("RefCounted object count: %u\n", GetRefCountedObjectCount());
#endif
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
        previous_frame_future = rhi.AdvanceFrame(previous_frame_sync_point.Raw());
        fflush(stdout);
        auto cpu_tp_end = std::chrono::steady_clock::now();
        cpu_duration = std::chrono::duration<float>(cpu_tp_end - cpu_tp_start).count();
    }
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
