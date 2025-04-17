/*
 * Created: 2025/4/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "renderer/mi_helpers.h"

#include <rhi/rhi.h>
#include <rhi/rhi_buffer.h>
MI_NAMESPACE_BEGIN

void Helpers::Upload(RHIBufferSpan buffer, const void *data, size_t size) {
    auto & rhi = RHI::Get();
    auto & queue = rhi.GetGraphicsCommandQueue();
    auto staging_buffer = rhi.CreateBuffer(size, RHIBufferUsageFlagBits::kStaging);
    auto staging_buffer_ptr = static_cast<uint8_t *>(staging_buffer->Map());
    memcpy(staging_buffer_ptr, data, size);
    staging_buffer->Unmap();
    queue.BufferBarrier(buffer, RHIPipelineStageFlagBits::kTransfer,
        RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kWrite);
    queue.CopyBuffer(staging_buffer->GetSpan(), buffer);
    queue.BufferBarrier(buffer, RHIPipelineStageFlagBits::kTransfer,
        RHIGPUAccessFlagBits::kWrite, RHIGPUAccessFlagBits::kAll);
}

void Helpers::UploadWithRDG(RenderGraphBuilder & builder, RHIBufferSpan buffer, const void * data, size_t size) {
    auto & rhi = RHI::Get();
    auto & queue = rhi.GetGraphicsCommandQueue();
    auto staging_buffer = rhi.CreateBuffer(size, RHIBufferUsageFlagBits::kStaging);
    auto staging_buffer_ptr = static_cast<uint8_t *>(staging_buffer->Map());
    memcpy(staging_buffer_ptr, data, size);
    staging_buffer->Unmap();


}

MI_NAMESPACE_END