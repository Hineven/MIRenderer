/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rhi/rhi.h"
#include "rhi/rhi_cmd.h"
#include "rhi/rhi_texture.h"

#ifdef MI_RHI_VK
// Not implemented yet
#include "vk/vk_rhi_export.h"
#endif

#ifdef MI_RHI_D3D12
#include "rhi/d3d12/d3d12_rhi.h"
#endif

MI_NAMESPACE_BEGIN

void RHI::AdvanceFrame(RHISyncPoint * sync_point) {
    auto & queue = GetGraphicsCommandQueue();

    // End the frame, and submit the command buffer
    queue.FrameEnd(false);
    queue.Submit(sync_point);
    // Swap allocators after the command buffer is submitted
    // The swapped out memory will last for about 1 frame more and silently be recycled
    RHI::Get().GetGraphicsCommandQueue().SwapAllocators();
    // Recycle resources that are pending for deletion
    RHI::Get().FlushRHIResourcesPendingForDeletion();
    // Increment the frame index kept by RHI thread.
    frame_index_ ++;
}

void RHI::FlushRHIResourcesPendingForDeletion(bool force) {
    // Delete the resources that are ready to be deleted in the queue.
    RHIResourceToRecycle resource {};
    while(!resources_pending_for_deletion_.empty()) {
        resource = resources_pending_for_deletion_.front();
        if(force || resource.frame_index < RHI::Get().GetFrameIndex() - 1) {
            delete resource.resource;
            resources_pending_for_deletion_.pop();
        } else {
            // The resource is not ready to be deleted, delay it to the next frame
            break;
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

void RHI::InitializeSingleton () {
    if(GDynamicRHI) {
        mi_assert(false, "RHI instance already created");
    }
    // Only D3D12 is supported for now
    GDynamicRHI = new D3D12RHI();
    GDynamicRHI->PostInitialize();
}

void RHI::DestroySingleton () {
    if(GDynamicRHI) {
        GDynamicRHI->WaitForIdle();
        // Recycle all pending resources before the real destruction of RHI.
        GDynamicRHI->FlushRHIResourcesPendingForDeletion(true);
        delete GDynamicRHI;
        GDynamicRHI = nullptr;
    }
}

bool RHI::HasSingleton() {
    return GDynamicRHI != nullptr;
}

RHI::RHI() {
    // Do nothing
}

RHI::~RHI() {
    // Make unique_ptr on incomplete type work
}

MI_NAMESPACE_END
