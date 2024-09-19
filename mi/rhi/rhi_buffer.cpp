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
        .num_slots = 1,
    };
    if(readonly) {
        bindless_slot_readonly_ = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHIBuffer>(desc);
        bindless_slot_readonly_->SetAndCommit(this);
    } else {
        bindless_slot_readwrite_ = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHIBuffer>(desc);
        bindless_slot_readwrite_->SetAndCommit(this);
    }
    bindless_ = true;
}

MI_NAMESPACE_END