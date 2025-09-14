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


void Start (std::unique_ptr<MIInfraInterface> && infra, const MainLoopStartConfig & cfg) {

    auto pwd = std::filesystem::current_path();

    // Transfer ownership of underlying infrastructure and initialize
    TransferInfra(std::move(infra));
    GetInfra().Init();

    if(GetCurrentThreadType() != ThreadType::kUnknown) {
        mi_assert(false, "MainLoop: somehow the thread calling Start() is known.");
    }
    SetCurrentThreadType(ThreadType::kRenderThread);

    // Initialize glfw
    glfwInit();

    // Initialize RHI
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
        sky_cube = TextureLoader::LoadEnvironmentMap("SkyTexture", GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/tief_etz_4k.png"));
    }

    auto default_mat = Material::Create("default_mat", {0.8f, 0.8f, 0.8f, 1.0f}, 1.0f, {0.0f, 0.0f, 0.0f});

    std::vector<TRef<StaticMeshInstance>> meshes;

    // Load internal models
    TRef<StaticMeshInstance> arrow_mesh_instance;
    {
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
            auto & r = Renderer::Get();
            arrow_mesh_instance = meshes.back();
            arrow_mesh_instance->GetStaticMesh()->SetRayTraced(false);
            // Switch to forward material
            auto & arrow_mats = arrow_mesh_instance->GetStaticMesh()->GetMaterials();
            assert(arrow_mats.size() == 1);
            auto arrow_mat = arrow_mats[0];
            arrow_mat->SetForward(true);
            arrow_mat->UpdateOnDevice(r.GetDeviceAllocator());
            arrow_mesh_instance->GetStaticMesh()->UpdateOnDevice(r.GetDeviceAllocator());
        }
    }

    // Load default model
    if (false) {
        std::vector<TRef<Geometry>> geometries;
        std::vector<TRef<Material>> materials;
        auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/light_room/scene.gltf");
        if (!GLTFLoader::LoadGLTF(
            model_path,
            *resource_allocator,
            *scene, nullptr,
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


    if (true) {
        std::vector<TRef<Geometry>> geometries;
        std::vector<TRef<Material>> materials;
        auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/cornell_box/scene.gltf");
        if (!GLTFLoader::LoadGLTF(
            model_path,
            *resource_allocator,
            *scene, nullptr,
            geometries, materials, meshes
        )) {
            MI_WARN("Failed to load GLTF model {}.", model_path.string());
        } else {
        }
        auto & r = Renderer::Get();
        for (auto e : meshes) {
            e->UpdateLights_Async(r.GetDeviceAllocator(), rhi.GetGraphicsCommandQueue());
        }
        TRef<VolumePrimitives> volprims;
        VolumePrimitivesLoader::LoadPLY(
            GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/puppy/point_cloud.ply"),
            *resource_allocator, volprims
        );
        if (volprims) {
            volprims->UpdateOnDevice(resource_allocator.get());
            auto volprims_instance = VolumePrimitivesInstance::Create(scene.get(), volprims.Raw(), Transform::FromMatrix(glm::mat4(1.0f)));
            volprims_instance->EditTransform().Translate({0, 0.5, 0});
        }
    }

    // Get ready for device rendering
    sky_cube->UpdateOnDevice();
    sky_cube->ConvertToBindless();

    scene->SetSkyCube(sky_cube.Raw());

    scene->CreateOnDevice();
    // Update is manually performed in the renderer.
    // scene->UpdateOnDevice();

    // View
    auto view = std::make_unique<RendererView>();
    view->film_width_ = cfg.window_width;
    view->film_height_ = cfg.window_height;
    view->scene_ = scene.get();

    // Keep track of selected renderable & primitive
    uint selected_renderable_index = UINT32_MAX;
    uint selected_primitive_index = UINT32_MAX;
    uint selected_descriptor_rank = UINT32_MAX;
    glm::vec2 selected_uv = {0.0f, 0.0f};

    {
        std::future<void> previous_frame_future;
        TRef<RHISyncPoint> previous_frame_sync_point = rhi.CreateSyncPoint();
        bool first_frame = true;

        // Main loop
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
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
                    // view->camera_.Up = glm::vec3(rotate_x * glm::vec4(view->camera_.Up, 0.0f));

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
            // UI
            {
                ImGui::Begin("Rendering");
                if (ImGui::Button("Reload Shaders") || should_reload_shaders) {
                    RHI::Get().WaitForIdle();
                    RDGShaderLibrary::Get().RecompileUpdatedCachedShaders();
                }
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
                ImGui::End();
            }
            // Render
            {
                RenderFrame(view.get(), pool.Raw());
            }

            // Click select
            auto & io = ImGui::GetIO();
            if (io.MouseClicked[0] && !io.WantCaptureMouse) {
                float mouse_x = io.MousePos.x;
                float mouse_y = io.MousePos.y;
                // 手动拷回Visibility
                auto rhi_visibility = view->G_visibility_->GetRHI();
                auto readback_buffer = rhi.CreateBuffer(
                    rhi_visibility->GetWidth() * rhi_visibility->GetHeight() * sizeof(uint32_t) * 4,
                    RHIBufferUsageFlagBits::kReadback
                );
                auto & queue = RHI::Get().GetGraphicsCommandQueue();
                // 管它丫儿地直接全部barrier
                queue.MemoryBarrier();
                // 转换layout
                queue.TextureBarrier(rhi_visibility, RHITextureLayoutType::kTransferSrcOptimal,
                    RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kAll,
                    RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kRead);
                // 拷贝
                queue.CopyTextureToBuffer(rhi_visibility, readback_buffer.Raw());
                // 因为这个纹理是RDG里面搞到的，得更新资源追踪
                view->G_visibility_->Use(
                    RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kTransferRead,
                    RHITextureLayoutType::kTransferSrcOptimal
                );
                // 以防万一（后面如果又有裸的CommandQueue调用），再Barrier一下
                queue.MemoryBarrier();
                // 等待
                queue.WaitForIdle("Readback Visibility");
                // 搞到buffer
                auto ptr = (glm::uvec4*)readback_buffer->Map();
                glm::uvec4 pixel = ptr[(int(mouse_y) * rhi_visibility->GetWidth() + int(mouse_x))];
                float uv_x = std::bit_cast<float>(pixel.z);
                float uv_y = std::bit_cast<float>(pixel.w);
                auto descriptor_rank = pixel.x >> 24;
                auto renderable_index = pixel.x & 0xFFFFFF;
                auto primitive_index = pixel.y;
                glm::vec2 uv = {uv_x, uv_y};
                if (selected_renderable_index != renderable_index) {
                    if (renderable_index == 0xFFFFFF) {
                        // Cancel selection, hide the arrow mesh
                        arrow_mesh_instance->SetVisible(false);
                    } else if (renderable_index != arrow_mesh_instance->GetIndex()) {
                        // Snap the arrow renderable to the selected renderable
                        auto renderable = scene->GetRenderables()[renderable_index].Raw();
                        arrow_mesh_instance->EditTransform().position = renderable->GetTransform().position;
                        arrow_mesh_instance->SetVisible(true);
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

            if (rhi.GetFrameIndex() % 1000 == 0) {
                printf("[%llu] Pool memory: %.2f MB\n", rhi.GetFrameIndex(), pool->GetTotalDeviceMemoryUsage() / 1024.0f / 1024.0f);
                fflush(stdout);
            }
            if (first_frame) {
                first_frame = false;
            } else {
                {
                    // PROFILE_SECTION(WaitPreviousFrame);
                    // Wait for the previous frame to finish execution on GPU before submitting commands about this frame
                    if (previous_frame_future.valid()) {
                        previous_frame_future.wait();
                    }
                }
                {
                    // PROFILE_SECTION(WaitFence);
                    // Wait for the previous frame to finish execution on GPU before submitting commands about this frame
                    previous_frame_sync_point->Wait();
                }
                previous_frame_sync_point->Reset();
            }
            {
                // PROFILE_SECTION(AdvanceFrame);
                // Submit commands recorded for this frame, and switch to next frame
                previous_frame_future = rhi.AdvanceFrame(previous_frame_sync_point.Raw());
            }
            fflush(stdout);
        }
    }


    RHI::Get().WaitForIdle();

    default_mat.SafeRelease();
    arrow_mesh_instance.SafeRelease();

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
    cfg.window_width = 1440;
    cfg.window_height = 900;

#ifndef NDEBUG
    // 仅用于Debug
    auto infra = std::make_unique<mi::MyInfra>(false, MI_PROJECT_ROOT);
#else
    auto infra = std::make_unique<mi::MyInfra>(true);
#endif
    mi::Start(std::move(infra), cfg);
}