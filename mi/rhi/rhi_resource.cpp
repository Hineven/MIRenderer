/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/infra.h"
#include "rhi/rhi.h"
#include "rhi/rhi_resource.h"
#include "util/lockfree.h"

MI_NAMESPACE_BEGIN

void RHIResource::QueueForDeletion() {
    CHECK_THREAD(RENDER);
    // This function lives in the render thread, so we use the frame index of the render thread.
    // It is always bigger than the frame index of the RHI thread.
    mi_assert(deletion_queue.Push({this, RHI::Get().GetFrameIndex()}), "Resource deletion queue overflow.");
}

MI_NAMESPACE_END