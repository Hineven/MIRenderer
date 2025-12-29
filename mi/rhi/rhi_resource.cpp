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

static std::atomic<uint64_t> g_living_rhi_resource_counter {0};

uint64_t RHIResource::GetLivingRHIResourceCount() {
#ifndef NDEBUG
    return g_living_rhi_resource_counter.load();
#else
    return 0;
#endif
}

RHIResource::RHIResource() {
#ifndef NDEBUG
    assert(GetCurrentThreadType() != ThreadType::kUnknown);
    owner_thread_ = GetCurrentThreadType();
    g_living_rhi_resource_counter.fetch_add(1);
#endif
}

RHIResource::~RHIResource () {
    // Make TRef compile
#ifndef NDEBUG
    g_living_rhi_resource_counter.fetch_sub(1);
#endif
}

void RHIResource::QueueForDeletion() {
    VerifyOwnerThread();
    CHECK_THREAD(ThreadType::kRenderThread, ThreadType::kRHIThread);
    mi_assert(RHI::HasSingleton(), "Potentially deleting a resource after RHI shutdown.");
    // This function lives in the render thread, so we use the frame index of the render thread.
    // It is always bigger than the frame index of the RHI thread.
    [[maybe_unused]] auto deleted = RHI::Get().AddResourcePendingForDeletion(this);
    mi_assert(deleted, "Resource deletion queue overflow.");
}

void RHIResource::SetName([[maybe_unused]] const std::string & name) {
#ifndef NDEBUG
    name_ = name;
#endif
}

float RHITimestamp::QueryTimestampInSeconds() const {
    float period = RHI::Get().GetDeviceProperties().timestamp_period;
    return float(double(QueryTimestamp()) * period * 1e-9f);
}


MI_NAMESPACE_END