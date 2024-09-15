/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rhi/rhi.h"
#include "rhi_cmd_exec.h"
#include "rhi/rhi_cmd.h"

// Import different kinds of RHI implementations
#include "vk/vk_rhi_export.h"

MI_NAMESPACE_BEGIN


std::future<void> RHI::AdvanceFrame() {
    auto & queue = RHICommandQueueGraphics::Get();
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

    // We can do this because there're only 1 RHI thread.
    queue.EnqueueTranslateAndSubmit();
    auto lambda = []() {
        // Swap allocators after the command buffer is submitted
        // The swapped out memory will last for about 1 frame more and silently be recycled
        RHICommandQueueGraphics::Get().SwapAllocators_RHIThread();
        // Recycle resources that are pending for deletion
        RHI::Get().RecycleRHIResourcesPendingForDeletion_RHIThread();
        // Increment the frame index kept by RHI thread.
        AdvanceFrame_RHIThread();
    };
    frame_index_ ++;
    return EnqueueRHIThreadTask(std::move(lambda));
}

void RHI::RecycleRHIResourcesPendingForDeletion_RHIThread() {
    bool removal = false;
    // Try to delete the resource that is not ready to be deleted in the previous frame first.
    if(remaining_resource_record_pending_for_deletion_.resource) {
        if(remaining_resource_record_pending_for_deletion_.frame_index < RHI::Get().GetFrameIndex()) {
            FreeResource_RHIThread(remaining_resource_record_pending_for_deletion_.resource);
            remaining_resource_record_pending_for_deletion_ = {};
            removal = true;
        }
    }
    // Delete the resources that are ready to be deleted in the queue.
    if(removal) {
        RHIResourceToRecycle resource {};
        while(resources_pending_for_deletion_.Pop(resource)) {
            if(resource.frame_index < RHI::Get().GetFrameIndex() - 1) {
                FreeResource_RHIThread(remaining_resource_record_pending_for_deletion_.resource);
            } else {
                // The resource is not ready to be deleted, delay it to the next frame;
                remaining_resource_record_pending_for_deletion_ = resource;
            }
        }
    }
}

static RHI * GDynamicRHI = nullptr;
static thread_local bool GIsRhiThread = false;

RHI & RHI::Get () {
    if(!GDynamicRHI) {
        mi_assert(false, "RHI instance not created");
    }
    return *GDynamicRHI;
}

bool IsRHIThread () {
    return GIsRhiThread;
}

void SetIsRHIThread (bool is_rhi_thread) {
    GIsRhiThread = is_rhi_thread;
}

void RHI::InitializeSingleton (RHIType type) {
    if(GDynamicRHI) {
        mi_assert(false, "RHI instance already created");
    }
    switch (type) {
        case RHIType::kVulkan:
            GDynamicRHI = reinterpret_cast<RHI *>(CreateVulkanRHIInstance());
            break;
        // ...
        default:
            mi_assert(false, "Unknown RHI type");
    }
}

void RHI::DestroySingleton () {
    if(GDynamicRHI) {
        GDynamicRHI->WaitForIdle();
        delete GDynamicRHI;
        GDynamicRHI = nullptr;
    }
}

MI_NAMESPACE_END
