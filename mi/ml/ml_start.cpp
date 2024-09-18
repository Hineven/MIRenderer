/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "ml/ml.h"
#include "core/task.h"
#include "rhi/rhi_thread.h"
#include "rhi/rhi.h"
#include "rhi/rhi_cmd.h"

MI_NAMESPACE_BEGIN

void MainLoop::Start () {
    if(GetCurrentThreadType() != ThreadType::kUnknown) {
        mi_assert(false, "Render thread started within a known thread.");
    }
    SetCurrentThreadType(ThreadType::kRenderThread);

    auto limits = infra_->GetResourceLimits();
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

    // Initialize RHI
    // We only have vulkan supported now.
    RHI::InitializeSingleton(RHIType::kVulkan);

    // Dispatch threads
    {
        // RHI thread
        auto result = infra_->LaunchThread(ThreadPerformanceType::kLow, [this] {
            StartAndRunRHIWorkerThread();
        });
        mi_assert(result, "Failed to launch RHI thread.");
        rhi_thread_ = std::move(result.value());
    }
    {
        // Render thread
        auto result = infra_->LaunchThread(ThreadPerformanceType::kHigh, [this] {
            Run();
        });
        mi_assert(result, "Failed to launch render thread.");
        render_thread_ = std::move(result.value());
    }
    MI_LOG(MIInfraLogType::kInfo, "Main loop initialization complete.");
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