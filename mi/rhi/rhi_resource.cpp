/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/infra.h"
#include "rhi/rhi.h"
#include "rhi/rhi_resource.h"
#include "core/util/lockfree.h"

MI_NAMESPACE_BEGIN

RHIResource::RHIResource() {}

RHIResource::~RHIResource () {
    // Make TRef compile
}

void RHIResource::QueueForDeletion() {
    CHECK_THREAD(RENDER);
    mi_assert(RHI::HasSingleton(), "Potentially deleting a resource after RHI shutdown.");
    // This function lives in the render thread, so we use the frame index of the render thread.
    // It is always bigger than the frame index of the RHI thread.
    RHI::Get().AddResourcePendingForDeletion(this);
}

MI_NAMESPACE_END