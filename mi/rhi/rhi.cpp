/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rhi/rhi.h"
#include "rhi_cmd_exec.h"
#include "rhi/rhi_cmd.h"
#include "rhi_bindless.h"
#include "rhi/rhi_texture.h"

// Import different kinds of RHI implementations
#include "vk/vk_rhi_export.h"

MI_NAMESPACE_BEGIN

std::future<void> RHI::AdvanceFrame() {
    auto & queue = GetGraphicsCommandQueue();
    // Detour the limitation that std function wrapper can not wrap non-copyable objects.
    // (Lambda capturing unmovable objects is not copyable)
//    int __index = (int)__tiny_buffer_for_hacking_[0];
//    __index = (__index + 1) % (std::size(__tiny_buffer_for_hacking_) - 1);
//    static_assert(sizeof(__tiny_buffer_for_hacking_) - 4 >= sizeof(std::future<void>) * 4);
//    auto fut_ptr = new(__tiny_buffer_for_hacking_ + __index * sizeof(std::future<void>) + 1)
//            std::future<void>(queue.EnqueueTranslateAndSubmit());
//    auto lambda = [fut_ptr]() {
//        fut_ptr->wait();
//        RHICommandQueueGraphics::Get().SwapAllocators_RHIThread();
//        fut_ptr->~future<void>();
//    };

    // We can do this because there are only 1 RHI thread.
    queue.FrameEnd(false);
    queue.EnqueueTranslateAndSubmit();
    auto lambda = []() {
        // Swap allocators after the command buffer is submitted
        // The swapped out memory will last for about 1 frame more and silently be recycled
        RHI::Get().GetGraphicsCommandQueue().SwapAllocators_RHIThread();
        // Swap the bindless descriptor set after the command buffer is submitted
        RHI::Get().GetBindlessManager().SwapSets_RHIThread();
        // Recycle resources that are pending for deletion
        RHI::Get().RecycleRHIResourcesPendingForDeletion_RHIThread();
        // Increment the frame index kept by RHI thread.
        AdvanceFrame_RHIThread();
    };
    frame_index_ ++;
    return EnqueueRHIThreadTask(std::move(lambda));
}

void RHI::RecycleRHIResourcesPendingForDeletion_RHIThread(bool force) {
    bool removal = true;
    // Try to delete the resource that is not ready to be deleted in the previous frame first.
    if(remaining_resource_record_pending_for_deletion_.resource) {
        removal = false;
        if(force || remaining_resource_record_pending_for_deletion_.frame_index < RHI::Get().GetFrameIndex()) {
            FreeResource_RHIThread(remaining_resource_record_pending_for_deletion_.resource);
            remaining_resource_record_pending_for_deletion_ = {};
            removal = true;
        }
    }
    // Delete the resources that are ready to be deleted in the queue.
    if(removal) {
        RHIResourceToRecycle resource {};
        while(resources_pending_for_deletion_.Pop(resource)) {
            if(force || resource.frame_index < RHI::Get().GetFrameIndex() - 1) {
                FreeResource_RHIThread(resource.resource);
            } else {
                // The resource is not ready to be deleted, delay it to the next frame;
                remaining_resource_record_pending_for_deletion_ = resource;
                break;
            }
        }
    }
}

static RHI * GDynamicRHI = nullptr;

RHI & RHI::Get () {
    if(!GDynamicRHI) {
        mi_assert(false, "RHI instance not created");
    }
    return *GDynamicRHI;
}

void RHI::InitializeSingleton (RHIType type) {
    if(GDynamicRHI) {
        mi_assert(false, "RHI instance already created");
    }
    switch (type) {
        case RHIType::kVulkan:
            GDynamicRHI = reinterpret_cast<RHI *>(CreateVulkanRHI());
            break;
        // ...
        default:
            mi_assert(false, "Unknown RHI type");
    }
    // Wait for the RHI thread to start
    // TODO this sucks
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    GDynamicRHI->PostInitialize();
}

void RHI::DestroySingleton () {
    if(GDynamicRHI) {
        GDynamicRHI->WaitForIdle();
        // Recycle all pending resources before the real destruction of RHI.
        EnqueueRHIThreadTask([](){
            RHI::Get().RecycleRHIResourcesPendingForDeletion_RHIThread(true);
        }).wait();
        delete GDynamicRHI;
        GDynamicRHI = nullptr;
    }
}

bool RHI::HasSingleton() {
    return GDynamicRHI != nullptr;
}

RHI::RHI() {
    auto res = GetInfra().LaunchThread(ThreadPerformanceType::kHigh, [this](){
        StartAndRunRHIWorkerThread();
    });
    mi_assert(res, "Failed to start RHI thread");
    rhi_thread_ = std::move(res.value());
}

RHI::~RHI() {
    // Stop RHI thread
    SignalStopRHIWorkerThreads();
    rhi_thread_->join();
    // Make unique_ptr on incomplete type work
}

MI_NAMESPACE_END
