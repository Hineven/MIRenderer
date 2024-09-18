/*
 * Created: 2024/9/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_buffer.h"
#include "rhi_bindless.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

void RHIBuffer::ConvertToBindless (bool readonly) {
    auto desc = RHIBindlessResourceDesc {
        .type = readonly ? RHIBindlessResourceType::kUniformBuffer : RHIBindlessResourceType::kStorageBuffer,
        .detail = {
            .buffer = this
        }
    };
    if(readonly) {
        bindless_slot_readonly_ = RHI::Get().GetBindlessManager().AllocateResourceSlot(desc);
    } else {
        bindless_slot_readwrite_ = RHI::Get().GetBindlessManager().AllocateResourceSlot(desc);
    }
    bindless_ = true;
}

MI_NAMESPACE_END