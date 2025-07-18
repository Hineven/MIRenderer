/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/infra.h"
#include "rhi/rhi.h"
#include "rhi/rhi_resource.h"
#include "core/util/queue.h"
#include "include/rhi/rhi_bindless.h"

MI_NAMESPACE_BEGIN

RHIResource::RHIResource() {
#ifndef NDEBUG
    assert(GetCurrentThreadType() != ThreadType::kUnknown);
    owner_thread_ = GetCurrentThreadType();
#endif
}

RHIResource::~RHIResource () {
    // Make TRef compile
}

void RHIResource::QueueForDeletion() {
    VerifyOwnerThread();
    CHECK_THREAD(ThreadType::kRenderThread, ThreadType::kRHIThread);
    mi_assert(RHI::HasSingleton(), "Potentially deleting a resource after RHI shutdown.");
    // This function lives in the render thread, so we use the frame index of the render thread.
    // It is always bigger than the frame index of the RHI thread.
    mi_assert(RHI::Get().AddResourcePendingForDeletion(this), "Resource deletion queue overflow.");
}

void RHIResource::SetName([[maybe_unused]] const std::string & name) {
    name_ = name;
    // Do nothing
}


MI_NAMESPACE_END