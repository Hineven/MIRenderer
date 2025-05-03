/*
 * Created: 2025/4/19
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_renderer_view.h"

#include "rdg/rdg_pool.h"
#include "rhi/rhi.h"
#include "rhi/rhi_buffer.h"
#include "rhi/rhi_desc.h"

MI_NAMESPACE_BEGIN

RHIBufferSpan RendererView::AllocateStagingBuffer(size_t size) {
    bool dedicated = false;
    if (size + staging_buffer_top_ > kStagingBufferDefaultSize) {
        if (size > kStagingBufferDefaultSize / 2) dedicated = true;
        else {
            staging_memory_footprint_ += kStagingBufferDefaultSize;
            staging_buffer_ = RHI::Get().CreateBuffer(kStagingBufferDefaultSize, RHIBufferUsageFlagBits::kStaging);
            staging_buffer_top_ = 0;
        }
    }
    if (dedicated) {
        staging_memory_footprint_ += size;
        return RHI::Get().CreateBuffer(size, RHIBufferUsageFlagBits::kStaging)->GetSpan();
    } else {
        auto buffer = staging_buffer_;
        auto offset = staging_buffer_top_;
        staging_buffer_top_ += size;
        return {buffer.Raw(), offset};
    }
}


MI_NAMESPACE_END