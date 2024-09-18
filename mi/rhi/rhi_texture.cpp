/*
 * Created: 2024/9/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_texture.h"
#include "rhi_bindless.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

void RHITexture::ConvertToBindless() {
    auto desc = RHIBindlessResourceDesc {
        .type = RHIBindlessResourceType::kSRV,
        .detail = {
            .texture = this
        }
    };
    bindless_slot_readonly_  = RHI::Get().GetBindlessManager().AllocateResourceSlot(desc);
    desc.type = RHIBindlessResourceType::kUAV;
    bindless_slot_readwrite_ = RHI::Get().GetBindlessManager().AllocateResourceSlot(desc);

    bindless_ = true;
}

MI_NAMESPACE_END