/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/infra.h"
#include "rhi/rhi.h"
#include "rhi/rhi_resource.h"
#include "core/util/lockfree.h"
#include "rhi_bindless.h"

MI_NAMESPACE_BEGIN


RHIResource::RHIResource() {}

RHIResource::~RHIResource () {
    // Make TRef compile
}

void RHIResource::QueueForDeletion() {
    CHECK_THREAD(RENDER);
    // This function lives in the render thread, so we use the frame index of the render thread.
    // It is always bigger than the frame index of the RHI thread.
    mi_assert(RHI::Get().AddResourcePendingForDeletion(this), "Resource deletion queue overflow.");
}

MI_NAMESPACE_END