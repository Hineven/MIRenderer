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
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
#include "renderer/mi_static_mesh.h"
#include "util/texture_loader.h"
#include "util/gltf_loader.h"

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
                RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kNone, RHIGPUAccessFlagBits::kWrite);
            cmd.CopyBufferToTexture(staging->GetSpan(), font_texture.Raw());
            cmd.TextureBarrier(font_texture.Raw(), RHITextureLayoutType::kShaderReadOnlyOptimal,
                RHIPipelineStageFlagBits::kAll, RHIGPUAccessFlagBits::kWrite, RHIGPUAccessFlagBits::kRead);
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
    auto resource_allocator = new CommonGroupedDeviceResourceAllocator(
        SimpleDeviceBufferHeap::Create(
            RHIBufferUsageFlagBits::kVertex, 256
        ).Raw(),
        SimpleDeviceBufferHeap::Create(
            RHIBufferUsageFlagBits::kIndex, 256
        ).Raw()
    );

    // Renderer
    Renderer::Get().Init(resource_allocator, pool.Raw());

    auto world = std::make_unique<RendererScene>();
    TRef<Texture> sky_cube;

    // Upload sky texture
    {
        sky_cube = TextureLoader::LoadEnvironmentMap("SkyTexture", GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/tief_etz_4k.png"));
    }

    std::vector<TRef<StaticMesh>> meshes;
    // Load default model
    {
        std::vector<TRef<Geometry>> geometries;
        std::vector<TRef<Material>> materials;
        auto model_path = GetInfra().TranslateResPathToFilePath("applications/3d_viewer/assets/bunny/scene.gltf");
        if (!GLTFLoader::LoadGLTF(
            model_path,
            *resource_allocator,
            *world,
            geometries, materials, meshes
        )) {
            MI_WARN("Failed to load GLTF model {}.", model_path.string());
        } else {
            for (auto e : meshes) {
                e->EditTransform().scale *= 0.1f; // Scale down the model
            }
        }
    }

    // Get ready for device rendering
    sky_cube->UpdateOnDevice();
    sky_cube->ConvertToBindless();

    world->SetSkyCube(sky_cube.Raw());

    // View
    auto view = std::make_unique<RendererView>();
    view->film_width_ = cfg.window_width;
    view->film_height_ = cfg.window_height;
    view->world_ = world.get();

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
                const float move_speed = 0.05f;
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
                ImGui::Text("Hello");
                if (ImGui::Button("Reload Shaders") || should_reload_shaders) {
                    RHI::Get().WaitForIdle();
                    RDGShaderLibrary::Get().RecompileUpdatedCachedShaders();
                }
                ImGui::End();
            }
            // Render
            {
                RenderFrame(view.get(), pool.Raw());
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
        }
    }


    RHI::Get().WaitForIdle();

    view.reset();

    meshes.clear();

    world.reset();

    sky_cube.SafeRelease();

    Renderer::DestroySingleton();

    assert(pool.GetRefCount() == 1);
    pool.SafeRelease();

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