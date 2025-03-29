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
#include <rhi/vk/vk_export.h>

// #include "core/task.h"
#include <rdg/rdg_pool.h>

#include "spinning_triangle.h"
#include "infra_impl/infra.h"
#include "rhi/rhi_thread.h"
#include "rhi/rhi.h"
#include "rhi/rhi_cmd.h"
#include "rdg/rdg_shader.h"
#include "core/util/debug_prof.h"

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

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // 为Vulkan准备

    std::string window_name = "MIRenderer";
    if (!cfg.window_name.empty()) window_name = cfg.window_name;
    GLFWwindow* window = glfwCreateWindow(cfg.window_width, cfg.window_height, window_name.c_str(), nullptr, nullptr);
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

    // Initialize the task graph singleton and its workers.
    // TaskGraph::InitializeSingleton(limits.max_low_performance_thread_count, task_graph_hpt_count);


    // Initialize shader library
    auto & shader_lib = RDGShaderLibrary::Get();
    shader_lib.Init();

    MI_LOG(MIInfraLogType::kInfo, "Main loop initialization complete.");

    // Set up window
    auto window = StartWindow(cfg);

    // 创建surface
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

    {
        std::future<void> previous_frame_future;
        TRef<RHISyncPoint> previous_frame_sync_point = rhi.CreateSyncPoint();
        bool first_frame = true;

        // Main loop
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            // Render
            {
                // PROFILE_SECTION(Rendering);
                RenderFrame(pool);
            }
            if (rhi.GetFrameIndex() % 20 == 0) {
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
            // We're using Vulkan, so we don't need to swap buffers
            // glfwSwapBuffers(window);
        }
    }


    RHI::Get().WaitForIdle();

    pool.SafeRelease();

    // TaskGraph::DestroySingleton();
    RDGShaderLibrary::DestroySingleton();
    RHI::DestroySingleton();

    GetInfra().Shutdown();
    DestroyInfra();

    glfwTerminate();

}

MI_NAMESPACE_END

int main () {
    mi::MainLoopStartConfig cfg;
    cfg.window_width = 800;
    cfg.window_height = 600;

    auto infra = std::make_unique<mi::MyInfra>();
    mi::Start(std::move(infra), cfg);
}