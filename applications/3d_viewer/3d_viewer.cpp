/*
 * Project Project: main.cpp
 * Created: 2024/6/27
 * This program uses MulanPSL2. See LICENSE for more.
 */

/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vulkan/vulkan.hpp"
#include <glfw/glfw3.h>
#include <imgui.h>

#include <rhi/vk/vk_export.h>
#include <rdg/rdg_pool.h>
#include <rhi/rhi_buffer.h>
#include <rhi/rhi_texture.h>

#include "3d_viewer.h"
#include "infra_impl/infra.h"
#include "rhi/rhi_thread.h"
#include "rhi/rhi.h"
#include "rhi/rhi_cmd.h"
#include "rdg/rdg_shader.h"
#include "core/util/debug_prof.h"
#include "imgui_impl_glfw.h"
#include "core/task.h"
#include "rdg/rdg.h"
#include "rdg/rdg_resource.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_cvar.h"
#include "util/texture_loader.h"
#include "util/gltf_loader.h"
#include "util/volprims_loader.h"

MI_NAMESPACE_BEGIN

struct MainLoopStartConfig {
    std::string window_name;
    uint32_t window_width;
    uint32_t window_height;
};

// Start a GLFW window with given configuration
GLFWwindow* StartWindow (const MainLoopStartConfig & cfg) {
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

// 主启动函数
void Start (std::unique_ptr<MIInfraInterface> && infra, const MainLoopStartConfig & cfg) {

	// 获取当前工作目录
    auto pwd = std::filesystem::current_path();

    // Transfer ownership of underlying infrastructure and initialize
    // 把注入的“基础设施”对象（资源路径、日志、平台能力等）转移到全局单例并初始化。
    TransferInfra(std::move(infra));
    GetInfra().Init();

    // 给当前线程打上“渲染线程”标签，方便后续做线程断言与调度。
    if(GetCurrentThreadType() != ThreadType::kUnknown) {
        mi_assert(false, "MainLoop: somehow the thread calling Start() is known.");
    }
    SetCurrentThreadType(ThreadType::kRenderThread);

    // Initialize glfw
    glfwInit();

    // Initialize RHI
	//向 GLFW 询问实例扩展
    //用扩展创建 RHI 
    //重置管线缓存
    {
        uint32_t extension_count = 0;
        auto extra_extensions =  glfwGetRequiredInstanceExtensions(&extension_count);
        if (extension_count != 0) {
            VulkanRHICreateInfo info {};
            info.extra_instance_extension_count = extension_count;
            info.extra_instance_extensions = extra_extensions;
            RHI::InitializeSingleton(RHIType::kVulkan, &info);
            RHI::Get().ResetPipelineCache(4 * 1024 * 1024); // Reset pipeline cache (at most 4MB)
        } else {
            throw std::runtime_error("Failed to get required instance extensions");
        }
    }

    auto limits = GetInfra().GetResourceLimits();

    // Initialize task graph
    if(limits.max_high_performance_thread_count < 2) {
        mi_assert(false, "At least 2 high performance threads are required.");
    }
    int task_graph_hpt_count = std::max(limits.max_high_performance_thread_count - 2, 0);
    int total_task_graph_thread_count = task_graph_hpt_count + limits.max_low_performance_thread_count;
    if(total_task_graph_thread_count < 1) {
        mi_assert(false, "At least 3 threads are required.");
    }

    TaskGraph::InitializeSingleton(0, task_graph_hpt_count);

    // Initialize the task graph singleton and its workers.
    // TaskGraph::InitializeSingleton(limits.max_low_performance_thread_count, task_graph_hpt_count);

    // Initialize shader library
    auto & shader_lib = RDGShaderLibrary::Get();
    shader_lib.Init();

    MI_LOG(MIInfraLogType::kInfo, "Main loop initialization complete.");

    // Set up window
    auto window = StartWindow(cfg);

    // Keep alive until window is closed
    RHITextureRef font_texture;

    // ImGui initialization
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForVulkan(window, true);
        // Upload fonts
        {
            unsigned char* pixels;
            int width, height;
            ImGuiIO& io = ImGui::GetIO();
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            auto & rhi = RHI::Get();
            // 创建字体纹理
            font_texture = rhi.CreateTexture(RHITextureType::k2D,
                {(uint32_t)width, (uint32_t)height, 1},
                PixelFormatType::kR8G8B8A8_UNORM,
                RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst);
            // 上传数据
            auto staging = rhi.CreateBuffer(width * height * 4, RHIBufferUsageFlagBits::kStaging);
            memcpy(staging->Map(), pixels, width * height * 4);
            auto  & cmd = rhi.GetGraphicsCommandQueue();
            cmd.TextureBarrier(font_texture.Raw(), RHITextureLayoutType::kTransferDstOptimal,
                RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kWrite);
            cmd.CopyBufferToTexture(staging->GetSpan(), font_texture.Raw());
            cmd.TextureBarrier(font_texture.Raw(), RHITextureLayoutType::kShaderReadOnlyOptimal,
                RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kWrite, RHIGPUAccessFlagBits::kRead);
            cmd.EnqueueTranslateAndSubmit();
            rhi.WaitForIdle();
            // 设置ImGui纹理ID
            io.Fonts->SetTexID((ImTextureID)font_texture.Raw());
            io.Fonts->ClearTexData(); // 清理CPU端数据
        }
    }

    // Swapchain & Surface
    VkSurfaceKHR surface_tmp;
    auto & rhi = RHI::Get();
    auto handles = static_cast<const VulkanRHIHandles*>(rhi.GetUnderlyingGraphicsAPIHandles());
    {
        auto result = glfwCreateWindowSurface(handles->instance, window, nullptr, &surface_tmp);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Failed to create window surface");
        }
    }
    rhi.InitializeSwapChain(&surface_tmp, cfg.window_width, cfg.window_height);

    auto pool = RDGResourcePool::Create();

    // Resource allocator
    auto resource_allocator = std::make_unique<DeviceBindlessResourceAllocator>();

    // Renderer
    Renderer::Get().Init(resource_allocator.get(), pool.Raw());

    auto scene = std::make_unique<Scene>();
    TRef<Texture> sky_cube;

    // Upload sky texture
    {
        sky_cube = TextureLoader::LoadEnvironmentMap("SkyTexture",
            GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/tief_etz_4k.exr"));
        // Get ready for device rendering
        sky_cube->UpdateOnDevice();
        sky_cube->ConvertToBindless();
    }

    auto default_mat = Material::Create("default_mat", {0.8f, 0.8f, 0.8f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});

    std::vector<TRef<StaticMeshInstance>> meshes;

    // Load internal models
    TRef<StaticMeshInstance> arrow_mesh_x_instance;
    TRef<StaticMeshInstance> arrow_mesh_y_instance;
    TRef<StaticMeshInstance> arrow_mesh_z_instance;
    TRef<StaticMesh> arrow_mesh_x;
    TRef<StaticMesh> arrow_mesh_y;
    TRef<StaticMesh> arrow_mesh_z;
    TRef<Geometry> arrow_geometry;
    {
        // arrow materials
        auto arrow_mat_x = Material::Create("arrow_mat_x", {1.0f, 0.0f, 0.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        auto arrow_mat_y = Material::Create("arrow_mat_y", {0.0f, 1.0f, 0.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        auto arrow_mat_z = Material::Create("arrow_mat_z", {0.0f, 0.0f, 1.0f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});
        arrow_mat_x->SetForward(true);
        arrow_mat_y->SetForward(true);
        arrow_mat_z->SetForward(true);
        auto & r = Renderer::Get();
        arrow_mat_x->UpdateOnDevice(r.GetDeviceAllocator());
        arrow_mat_y->UpdateOnDevice(r.GetDeviceAllocator());
        arrow_mat_z->UpdateOnDevice(r.GetDeviceAllocator());
        TRef<StaticMeshInstance> original_arrow_instance;
        // arrow
        {
            std::vector<TRef<Geometry>> geometries;
            std::vector<TRef<Material>> materials;
            auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/internal/arrow.gltf");
            if (!GLTFLoader::LoadGLTF(
                model_path,
                *resource_allocator,
                *scene, default_mat.Raw(),
                geometries, materials, meshes
            )) {
                MI_WARN("Failed to load GLTF model {}.", model_path.string());
            } else {
            }
            original_arrow_instance = meshes.back();
            auto mesh = original_arrow_instance->GetStaticMesh();
            // Extract geometry
            arrow_geometry = mesh->GetGeometries()[0];
        }
        // Create instances
        {
            float scale = 0.25f;
            arrow_mesh_x = StaticMesh::Create(false, false);
            arrow_mesh_x->AddMeshPrimitive(arrow_geometry.Raw(), arrow_mat_x.Raw());
            arrow_mesh_x->UpdateOnDevice(r.GetDeviceAllocator());
            arrow_mesh_x_instance = StaticMeshInstance::Create(scene.get(), arrow_mesh_x.Raw(),
                original_arrow_instance->GetTransform().Scaled(glm::vec3(scale)));

            arrow_mesh_y = StaticMesh::Create(false, false);
            arrow_mesh_y->AddMeshPrimitive(arrow_geometry.Raw(), arrow_mat_y.Raw());
            arrow_mesh_y->UpdateOnDevice(r.GetDeviceAllocator());
            // Rotate along z for 90 degrees : x->y axis
            arrow_mesh_y_instance = StaticMeshInstance::Create(scene.get(), arrow_mesh_y.Raw(),
                original_arrow_instance->GetTransform().RotatedAbout(glm::radians(90.0f), {0, 0, 1}).Scaled(glm::vec3(scale)));

            arrow_mesh_z = StaticMesh::Create(false, false);
            arrow_mesh_z->AddMeshPrimitive(arrow_geometry.Raw(), arrow_mat_z.Raw());
            arrow_mesh_z->UpdateOnDevice(r.GetDeviceAllocator());
            // Rotate along y for -90 degrees : x->z axis
            arrow_mesh_z_instance = StaticMeshInstance::Create(scene.get(), arrow_mesh_z.Raw(),
                original_arrow_instance->GetTransform().RotatedAbout(glm::radians(-90.0f), {0, 1, 0}).Scaled(glm::vec3(scale)));
        }
        // Remove original arrow from scene
        scene->RemoveRenderable(original_arrow_instance.Raw());
        // Set invisible at start
        arrow_mesh_x_instance->SetVisible(false);
        arrow_mesh_y_instance->SetVisible(false);
        arrow_mesh_z_instance->SetVisible(false);
    }

    // Load default model
    if (true) {
        std::vector<TRef<Geometry>> geometries;
        std::vector<TRef<Material>> materials;
        // auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/light_room/scene.gltf");
        // auto model_path = std::filesystem::path("D:/TestScene/remi-room/RemiIndoorsHard.gltf");
        auto model_path = std::filesystem::path("D:/TestScene/room/Room.gltf");
        // auto model_path = std::filesystem::path("D:/TestScene/BugTest/Bug.gltf");
        if (!GLTFLoader::LoadGLTF(
            model_path,
            *resource_allocator,
            *scene, default_mat.Raw(),
            geometries, materials, meshes
        )) {
            MI_WARN("Failed to load GLTF model {}.", model_path.string());
        } else {
        }
        auto & r = Renderer::Get();
        for (auto e : meshes) {
            e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
        }
    }


    if (false) {
        std::vector<TRef<Geometry>> geometries;
        std::vector<TRef<Material>> materials;
        auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/light_room/scene.gltf");
        if (!GLTFLoader::LoadGLTF(
            model_path,
            *resource_allocator,
            *scene, default_mat.Raw(),
            geometries, materials, meshes
        )) {
            MI_WARN("Failed to load GLTF model {}.", model_path.string());
        } else {
        }
        auto & r = Renderer::Get();
        for (auto e : meshes) {
            e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
        }
        // TRef<VolumePrimitives> volprims;
        // VolumePrimitivesLoader::LoadPLY(
        //     GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/puppy/point_cloud.ply"),
        //     *resource_allocator, volprims
        // );
        // if (volprims) {
        //     volprims->UpdateOnDevice(resource_allocator.get());
        //     auto volprims_instance = VolumePrimitivesInstance::Create(scene.get(), volprims.Raw(), Transform::FromMatrix(glm::mat4(1.0f)));
        //     volprims_instance->EditTransform().Translate({0, 0.5, 0});
        // }
    }

    scene->SetSkyCube(sky_cube.Raw());

    scene->CreateOnDevice();
    // Update is manually performed in the renderer.
    // scene->UpdateOnDevice();

    // View
    auto view = std::make_unique<RendererView>();
    view->film_width_ = cfg.window_width;
    view->film_height_ = cfg.window_height;
    view->scene_ = scene.get();

    // Setup default directional light
    scene->directional_light_.direction = glm::normalize(glm::vec3(-5.5f, -4.4f, 5.5f));

    // Keep track of selected renderable & primitive
    uint selected_renderable_index = UINT32_MAX;
    uint selected_primitive_index = UINT32_MAX;
    uint selected_descriptor_rank = UINT32_MAX;
    uint selected_deferred_renderable_index = UINT32_MAX;
    glm::vec2 selected_uv = {0.0f, 0.0f};
    float cpu_duration = 0;


    std::vector<RDGTimePeriod> time_periods;

    {
        std::future<void> previous_frame_future;
        TRef<RHISyncPoint> previous_frame_sync_point = rhi.CreateSyncPoint();
        bool first_frame = true;

        // Main loop
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            auto cpu_tp_start = std::chrono::steady_clock::now();
            bool should_reload_shaders = false;
            // ImGui new frame routine
            {
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
            }
            // Camera control
            {
                // 相机移动参数
                const float move_speed = 0.02f;
                const float mouse_sensitivity = 0.002f;
                glm::vec3 camera_right = glm::normalize(glm::cross(view->camera_.direction, glm::vec3(0.0f, 1.0f, 0.0f)));
                // 获取键盘输入控制移动
                if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
                    view->camera_.position += view->camera_.direction * move_speed;
                if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
                    view->camera_.position -= view->camera_.direction * move_speed;
                if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
                    view->camera_.position -= camera_right * move_speed;
                if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
                    view->camera_.position += camera_right * move_speed;
                if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
                    view->camera_.position += view->camera_.up * move_speed;
                if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
                    view->camera_.position -= view->camera_.up * move_speed;

                // 鼠标控制视角旋转
                static double last_mouse_x = 0.0, last_mouse_y = 0.0;
                static bool first_mouse = true;

                double mouse_x, mouse_y;
                glfwGetCursorPos(window, &mouse_x, &mouse_y);

                if (first_mouse) {
                    last_mouse_x = mouse_x;
                    last_mouse_y = mouse_y;
                    first_mouse = false;
                }

                if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
                    // 计算鼠标偏移量
                    float delta_x = static_cast<float>(mouse_x - last_mouse_x) * mouse_sensitivity;
                    float delta_y = static_cast<float>(mouse_y - last_mouse_y) * mouse_sensitivity;

                    // 更新相机方向
                    // 水平旋转（偏航角）
                    glm::mat4 rotate_y = glm::rotate(glm::mat4(1.0f), -delta_x, glm::vec3(0, 1, 0));
                    view->camera_.direction = glm::vec3(rotate_y * glm::vec4(view->camera_.direction, 0.0f));

                    // 垂直旋转（俯仰角）- 围绕右向量旋转
                    glm::mat4 rotate_x = glm::rotate(glm::mat4(1.0f), -delta_y, camera_right);
                    view->camera_.direction = glm::vec3(rotate_x * glm::vec4(view->camera_.direction, 0.0f));

                    // 确保所有向量都是单位向量
                    view->camera_.direction = glm::normalize(view->camera_.direction);
                }

                last_mouse_x = mouse_x;
                last_mouse_y = mouse_y;
            }
            // Hotkeys
            {
                // F5: Reload shaders
                if (glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS) {
                    should_reload_shaders = true;
                }
            }

            auto & io = ImGui::GetIO();
            // Update mouse pos cvars
            {
                CVar_DebugCursorScreenCoordsX.Set((int)round(io.MousePos.x));
                CVar_DebugCursorScreenCoordsY.Set((int)round(io.MousePos.y));
            }

            // UI
            {
                ImGui::Begin("Rendering");
                if (ImGui::Button("Reload Shaders") || should_reload_shaders) {
                    RHI::Get().WaitForIdle();
                    RDGShaderLibrary::Get().RecompileUpdatedCachedShaders();
                }
                if (ImGui::CollapsingHeader("Selected Renderable")) {
                    if (selected_deferred_renderable_index != UINT32_MAX) {
                        auto renderable = scene->GetRenderables()[selected_deferred_renderable_index];
                        ImGui::Text("Index: %d", renderable->GetIndex());
                        ImGui::Text("Type: %s", ToString(renderable->GetType()).c_str());
                        Transform & t = renderable->EditTransform();
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
                            for (auto r : scene->GetRenderables()) {
                                if (r) r->SetVisible(true);
                            }
                        }
                    }
                }
                if (ImGui::CollapsingHeader("CVars")) {
                    // CVars
                    auto & cvar_registry = CVarRegistry::GetInstance();
                    for (auto e : cvar_registry.GetAllCVars()) {
                        if (e->GetType() == CVarType::kBool) {
                            auto cvar = static_cast<CVar<bool>*>(e);
                            bool value = cvar->Get();
                            if (ImGui::Checkbox(cvar->GetId().c_str(), &value)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kFloat) {
                            auto cvar = static_cast<CVar<float>*>(e);
                            float value = cvar->Get();
                            if (ImGui::DragFloat(cvar->GetId().c_str(), &value, 0.01f)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kFloat2) {
                            auto cvar = static_cast<CVar<glm::vec2>*>(e);
                            glm::vec2 value = cvar->Get();
                            if (ImGui::DragFloat2(cvar->GetId().c_str(), &value[0], 0.01f)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kFloat3) {
                            auto cvar = static_cast<CVar<glm::vec3>*>(e);
                            glm::vec3 value = cvar->Get();
                            if (ImGui::DragFloat3(cvar->GetId().c_str(), &value[0], 0.01f)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kFloat4) {
                            auto cvar = static_cast<CVar<glm::vec4>*>(e);
                            glm::vec4 value = cvar->Get();
                            if (ImGui::DragFloat4(cvar->GetId().c_str(), &value[0], 0.01f)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kInt) {
                            auto cvar = static_cast<CVar<int>*>(e);
                            int value = cvar->Get();
                            if (ImGui::DragInt(cvar->GetId().c_str(), &value)) {
                                cvar->Set(value);
                            }
                        } else if (e->GetType() == CVarType::kString) {
                            auto cvar = static_cast<CVar<std::string>*>(e);
                            std::string value = cvar->Get();
                            char buffer[256];
                            strncpy_s(buffer, value.c_str(), sizeof(buffer));
                            if (ImGui::InputText(cvar->GetId().c_str(), buffer, sizeof(buffer))) {
                                cvar->Set(std::string(buffer));
                            }
                        }
                    }
                }
                if (ImGui::CollapsingHeader("Performance")) {
                    ImGui::Text("CPU: %.2f ms", cpu_duration * 1000.0);
                    // 构建树状结构
                    std::function<void(int, int, int)> DrawTree;
                    DrawTree = [&](int start, int end, int depth) {
                        ImGui::Indent(20);
                        int last = start;
                        for (int i = start; i < end; i++) {
                            if (time_periods[i].class_names.size() <= depth
                            ||  time_periods[i].class_names[depth] != time_periods[last].class_names[depth]) {
                                if (last != i) {
                                    // Tree node
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
                                    // Leaf node
                                    std::string node_name = time_periods[i].pass_name;
                                    ImGui::Text("%s: %.2f ms", node_name.c_str(), time_periods[i].duration * 1000);
                                }
                                last = i + 1;
                            }
                        }
                            // Tree node
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
            // Render
            {
                RenderGraphBuilder builder;
                RenderFrame(builder, view.get());

                if (io.MouseClicked[0] && !io.WantCaptureMouse) {
                    // Export forward depth and visibility for later use
                    view->forward_depth_->SetExport();
                    view->G_visibility_->SetExport();
                }

                std::string frame_name = "Frame " + std::to_string(GetFrameIndexForCurrentThread());
                auto graph = builder.Compile(frame_name);
                graph->Execute(pool.Raw());
                time_periods = graph->GetTimestampPeriods();
            }

            // Click select
            static float last_click_forward_depth = 0;
            if (io.MouseClicked[0] && !io.WantCaptureMouse) {
                float mouse_x = io.MousePos.x;
                float mouse_y = io.MousePos.y;
                // 手动拷回Visibility和Depth
                auto rhi_visibility = view->G_visibility_->GetRHI();
                auto rhi_fwd_depth = view->forward_depth_->GetRHI();
                auto readback_buffer_visibility = rhi.CreateBuffer(
                    rhi_visibility->GetWidth() * rhi_visibility->GetHeight() * sizeof(uint32_t) * 4,
                    RHIBufferUsageFlagBits::kReadback
                );
                auto readback_buffer_depth = rhi.CreateBuffer(
                    rhi_fwd_depth->GetWidth() * rhi_fwd_depth->GetHeight() * sizeof(float),
                    RHIBufferUsageFlagBits::kReadback
                );
                auto & queue = RHI::Get().GetGraphicsCommandQueue();
                // 管它丫儿地直接全部barrier
                queue.MemoryBarrier();
                // 转换layout
                queue.TextureBarrier(rhi_visibility, RHITextureLayoutType::kTransferSrcOptimal,
                    RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
                    RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
                queue.TextureBarrier(rhi_fwd_depth, RHITextureLayoutType::kTransferSrcOptimal,
                    RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
                    RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
                // 拷贝
                queue.CopyTextureToBuffer(rhi_visibility, readback_buffer_visibility.Raw());
                queue.CopyTextureToBuffer(rhi_fwd_depth, readback_buffer_depth.Raw());
                // 因为这个纹理是RDG里面搞到的，RDG外手操之后得更新资源追踪
                view->G_visibility_->Use(
                    RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kTransferRead,
                    RHITextureLayoutType::kTransferSrcOptimal
                );
                view->forward_depth_->Use(
                    RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kTransferRead,
                    RHITextureLayoutType::kTransferSrcOptimal
                );
                // 以防万一（后面如果又有裸的CommandQueue调用），再Barrier一下
                queue.MemoryBarrier();
                // 等待
                queue.WaitForIdle("Readback Buffers");
                // 搞到buffer
                auto ptr = (glm::uvec4*)readback_buffer_visibility->Map();
                glm::uvec4 pixel = ptr[(int(mouse_y) * rhi_visibility->GetWidth() + int(mouse_x))];
                auto depth_ptr = (float*)readback_buffer_depth->Map();
                last_click_forward_depth = depth_ptr[(int(mouse_y) * rhi_fwd_depth->GetWidth() + int(mouse_x))];
                readback_buffer_visibility->Unmap();
                readback_buffer_depth->Unmap();
                float uv_x = std::bit_cast<float>(pixel.z);
                float uv_y = std::bit_cast<float>(pixel.w);
                auto descriptor_rank = pixel.x >> 24;
                auto renderable_index = pixel.x & 0xFFFFFF;
                if (renderable_index == 0xFFFFFF) {
                    // Clicked on background
                    renderable_index = UINT32_MAX;
                }
                auto primitive_index = pixel.y;
                glm::vec2 uv = {uv_x, uv_y};
                if (selected_renderable_index != renderable_index) {
                    if (renderable_index == UINT32_MAX) {
                        // Cancel selection, hide the arrow mesh
                        arrow_mesh_x_instance->SetVisible(false);
                        arrow_mesh_y_instance->SetVisible(false);
                        arrow_mesh_z_instance->SetVisible(false);
                        // Remove deferred renderable selection
                        selected_deferred_renderable_index = UINT32_MAX;
                    } else if (renderable_index != arrow_mesh_x_instance->GetIndex()
                        && renderable_index != arrow_mesh_y_instance->GetIndex()
                        && renderable_index != arrow_mesh_z_instance->GetIndex()
                    ) {
                        // Selected a deferred renderable
                        selected_deferred_renderable_index = renderable_index;
                        // Snap the arrow renderable to the selected renderable
                        auto renderable = scene->GetRenderables()[renderable_index].Raw();
                        arrow_mesh_x_instance->EditTransform().position = renderable->GetTransform().position;
                        arrow_mesh_x_instance->SetVisible(true);
                        arrow_mesh_y_instance->EditTransform().position = renderable->GetTransform().position;
                        arrow_mesh_y_instance->SetVisible(true);
                        arrow_mesh_z_instance->EditTransform().position = renderable->GetTransform().position;
                        arrow_mesh_z_instance->SetVisible(true);
                    }
                }
                selected_descriptor_rank = descriptor_rank;
                selected_renderable_index = renderable_index;
                selected_primitive_index = primitive_index;
                selected_uv = uv;
                MI_LOG(MIInfraLogType::kInfo, "Selected Renderable {}, Primitive {}, Descriptor Rank {}, UV ({}, {})",
                    selected_renderable_index, selected_primitive_index, selected_descriptor_rank, selected_uv.x, selected_uv.y
                );
            }

            // Left mouse Drag
            static glm::vec2 drag_mouse_start_pos = {};
            static glm::vec3 drag_start_obj_pos = {};
            static bool dragging = false;
            if (io.MouseDown[GLFW_MOUSE_BUTTON_LEFT]) {
                int axis = -1;
                if (selected_renderable_index == arrow_mesh_x_instance->GetIndex()) axis = 0;
                else if (selected_renderable_index == arrow_mesh_y_instance->GetIndex()) axis = 1;
                else if (selected_renderable_index == arrow_mesh_z_instance->GetIndex()) axis = 2;
                if (axis != -1) {
                    glm::vec2 mouse;
                    mouse.x = io.MousePos.x;
                    mouse.y = io.MousePos.y;
                    // Only when the UI is not blocking the mouse can the dragging start
                    if (!dragging && !io.WantCaptureMouse) {
                        drag_mouse_start_pos = mouse;
                        // Save the start object center
                        if (selected_deferred_renderable_index != UINT32_MAX) {
                            auto renderable = scene->GetRenderables()[selected_deferred_renderable_index].Raw();
                            drag_start_obj_pos = renderable->GetTransform().position;
                        }
                        dragging = true;
                    }
                    if (dragging) {
                        glm::vec3 end_world_pos {};
                        {
                            // 先计算点击原始开始坐标
                            auto & camera = view->camera_;
                            glm::vec2 ndc2 = {
                                (drag_mouse_start_pos.x / view->film_width_) * 2.0f - 1.0f,
                                1.0f - (drag_mouse_start_pos.y / view->film_height_) * 2.0f
                            };
                            float linear_depth = camera.ReversedZDepthToLinearDepth(last_click_forward_depth);
                            float aspect = float(view->film_width_) / float(view->film_height_);
                            glm::vec3 start_world_pos = camera.RecoverWorldPositionNDC2(ndc2, linear_depth, aspect);
                            glm::vec2 curr_ndc2 = {
                                (mouse.x / view->film_width_) * 2.0f - 1.0f,
                                1.0f - (mouse.y / view->film_height_) * 2.0f
                            };
                            glm::vec3 cursor_end_world_pos = camera.RecoverWorldPositionNDC2(curr_ndc2, linear_depth, aspect);
                            glm::vec3 delta = cursor_end_world_pos - start_world_pos;
                            glm::vec3 delta_projected = {};
                            delta_projected[axis] = delta[axis];
                            end_world_pos = drag_start_obj_pos + delta_projected;
                        }
                        if (selected_deferred_renderable_index != UINT32_MAX) {
                            auto renderable = scene->GetRenderables()[selected_deferred_renderable_index].Raw();
                            renderable->EditTransform().position = end_world_pos;
                            // Snap the arrow renderables to the updated position
                            arrow_mesh_x_instance->EditTransform().position = end_world_pos;
                            arrow_mesh_y_instance->EditTransform().position = end_world_pos;
                            arrow_mesh_z_instance->EditTransform().position = end_world_pos;
                        }
                    }
                } else {
                    dragging = false;
                }
            } else {
                dragging = false;
            }

            if (rhi.GetFrameIndex() % 1000 == 0) {
                printf("[%llu] Pool memory: %.2f MB\n", rhi.GetFrameIndex(), pool->GetTotalDeviceMemoryUsage() / 1024.0f / 1024.0f);
                fflush(stdout);
            }
            if (first_frame) {
                first_frame = false;
            } else {
                {
                    // Wait for the previous frame to finish execution on CPU
                    if (previous_frame_future.valid()) {
                        previous_frame_future.wait();
                    }
                }
                {
                    // Wait for the previous frame to finish execution on GPU
                    previous_frame_sync_point->Wait();
                }
                previous_frame_sync_point->Reset();
            }
            {
                // Finish this frame, clear double-buffered resources and switch to next frame
                previous_frame_future = rhi.AdvanceFrame(previous_frame_sync_point.Raw());
            }
            fflush(stdout);
            auto cpu_tp_end = std::chrono::steady_clock::now();
            cpu_duration = std::chrono::duration<float>(cpu_tp_end - cpu_tp_start).count();
        }
    }


    RHI::Get().WaitForIdle();

    default_mat.SafeRelease();
    {
        arrow_mesh_x_instance.SafeRelease();
        arrow_mesh_y_instance.SafeRelease();
        arrow_mesh_z_instance.SafeRelease();
        arrow_mesh_x.SafeRelease();
        arrow_mesh_y.SafeRelease();
        arrow_mesh_z.SafeRelease();
        arrow_geometry.SafeRelease();
    }

    view.reset();

    meshes.clear();

    scene.reset();

    sky_cube.SafeRelease();

    Renderer::DestroySingleton();

    assert(pool.GetRefCount() == 1);
    pool.SafeRelease();

    resource_allocator.release();

    RDGShaderLibrary::Get().Deinit();

    TaskGraph::DestroySingleton();

    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    font_texture.SafeRelease();

    RHI::DestroySingleton();

    GetInfra().Shutdown();
    DestroyInfra();

    glfwTerminate();
}

MI_NAMESPACE_END

int main () {
    mi::MainLoopStartConfig cfg;
    cfg.window_width = 1920;
    cfg.window_height = 1080;

#ifndef NDEBUG
    // 仅用于Debug
    auto infra = std::make_unique<mi::MyInfra>(false, MI_PROJECT_ROOT);
#else
    auto infra = std::make_unique<mi::MyInfra>(true);
#endif
    mi::Start(std::move(infra), cfg);
}