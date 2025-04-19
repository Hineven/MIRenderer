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
#include "rhi/rhi_buffer.h"
#include "rhi/rhi_thread.h"

// Import different kinds of RHI implementations
#include "vk/vk_rhi_export.h"

MI_NAMESPACE_BEGIN
    size_t GetFrameIndexForCurrentThread() {
    if (GetCurrentThreadType() == ThreadType::kRHIThread) {
        return GetCurrentFrameIndex_RHIThread();
    } else {
        return RHI::Get().GetFrameIndex();
    }
}

RHITextureRef RHI::CreateTexture(RHITextureType type, RHITextureDimensions dimensions,
    PixelFormatType format, RHITextureUsageFlags usage, uint32_t mip_levels, uint32_t array_layers) {
    return CreateTexture({type, dimensions, mip_levels, array_layers, format, usage});
}

RHIBufferRef RHI::CreateBuffer(size_t size, RHIBufferUsageFlags type) {
    return CreateBuffer({size, type});
}

bool RHI::InitializeSwapChain(const void *surface_handle_ptr, uint32_t width, uint32_t height) {
    assert(!is_swapchain_initialized_ && "Double initialization of swapchain");
    if (InitializeSwapChain_RHI(surface_handle_ptr, width, height, &swapchain_size_)) {
        is_swapchain_initialized_ = true;
    }
    return is_swapchain_initialized_;
}

// This can not be called if the frame before the current frame (which is ending) is not finished yet.
std::future<void> RHI::AdvanceFrame(RHISyncPoint * sync_point) {
    auto & queue = GetGraphicsCommandQueue();

    // Translate and submit all commands, present the backbuffer, step to the next frame.
    queue.FrameEnd(sync_point);
    // Swap allocators after the termination of this frame. No new commands shall be allocated with the old allocators.
    // The swapped out memory will last for about 1 frame more and silently be recycled
    queue.SwapAllocators();
    auto lambda = []() {
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
            delete remaining_resource_record_pending_for_deletion_.resource;
            remaining_resource_record_pending_for_deletion_ = {};
            removal = true;
        }
    }
    // Delete the resources that are ready to be deleted in the queue.
    if(removal) {
        RHIResourceToRecycle resource {};
        while(resources_pending_for_deletion_.Pop(resource)) {
            if(force || resource.frame_index < RHI::Get().GetFrameIndex() - 1) {
                delete resource.resource;
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

void RHI::InitializeSingleton (RHIType type, const void * extra) {
    if(GDynamicRHI) {
        mi_assert(false, "RHI instance already created");
    }
    switch (type) {
        case RHIType::kVulkan:
            GDynamicRHI = reinterpret_cast<RHI *>(CreateVulkanRHI((const VulkanRHICreateInfo*)extra));
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

void RHI::PostInitialize() {
    // Create global samplers
    {
        auto linear_wrap = CreateSampler(RHISamplerFilterType::kLinear, RHISamplerAddressModeType::kRepeat);
        linear_wrap->IncRef();
        global_samplers_.linear_wrap = linear_wrap.Raw();
    }
}


void RHI::DestroySingleton () {
    if(GDynamicRHI) {
        GDynamicRHI->WaitForIdle();
        GDynamicRHI->PreDestruction();
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

void RHI::PreDestruction () {
    // Release samplers
    global_samplers_.linear_wrap->DecRef();
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
    // Make unique_ptr on incomplete types work
}

MI_NAMESPACE_END
