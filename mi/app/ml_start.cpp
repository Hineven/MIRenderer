/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "../rdg/include/rdg/rdg_shader.h"
#include "ml/ml.h"
#include "core/task.h"
#include "rhi/rhi_thread.h"
#include "rhi/rhi.h"
#include "rhi/rhi_cmd.h"

MI_NAMESPACE_BEGIN

void MainLoop::Start (std::unique_ptr<MIInfraInterface> && infra, MainLoopStartConfig cfg) {

    static MainLoop * instance_ = nullptr;
    if (instance_ != nullptr) {
        MI_WARN("Potentially double calling MainLoop::Start()");
        return ;
    } else {
        instance_ = new MainLoop();
    }

    auto pwd = std::filesystem::current_path();

    // Transfer ownership of underlying infrastructure and initialize
    TransferInfra(std::move(infra));
    GetInfra().Init();

    instance_->config_ = cfg;

    if(GetCurrentThreadType() != ThreadType::kUnknown) {
        mi_assert(false, "MainLoop: somehow the thread calling Start() is known.");
    }
    SetCurrentThreadType(ThreadType::kRenderThread);
    RHI::InitializeSingleton(RHIType::kVulkan);

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
    TaskGraph::InitializeSingleton(limits.max_low_performance_thread_count, task_graph_hpt_count);

    // Initialize shader library
    auto & shader_lib = RDGShaderLibrary::Get();
    shader_lib.Init();

    // Set up window
    instance_->StartWindow();

    MI_LOG(MIInfraLogType::kInfo, "Main loop initialization complete.");
}

void MainLoop::StartWindow() {

}



void MainLoop::Run () {
    // Main rendering loop
    while(true) {
        // Swap host allocators for the queue.
        // The future returns once the frame is submitted to the device but may not yet being executed.
        auto frame_advancing = RHI::Get().AdvanceFrame();
        frame_advancing.wait();

        auto builder = RenderGraph::CreateBuilder();

        // Execute the geometry tick and possibly record RDG tasks.
        RunGeometryTick();
    }
}

void MainLoop::SynchronizeFrame () {
    // Synchronize a frame (block until the render commands for the previous frame is submitted and the next frame
    // is ready for recording.)
}

MI_NAMESPACE_END