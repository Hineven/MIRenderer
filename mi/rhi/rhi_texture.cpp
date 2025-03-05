/*
 * Created: 2024/9/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_texture.h"
#include "rhi_bindless.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

RHITexture::RHITexture(RHITextureDesc desc): desc_(desc) {}

RHITexture::~RHITexture() {

}

void RHITexture::ConvertToBindless(bool optimal_accessed) {
    is_bindless_optimal_accessed_ = optimal_accessed;
    auto desc = RHIBindlessResourceDesc {.type = RHIBindlessResourceType::kSRV};
    bindless_slot_readonly_  = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>(desc);
    desc.type = RHIBindlessResourceType::kUAV;
    bindless_slot_readwrite_ = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>(desc);
    bindless_slot_readonly_->SetAndCommit(this);
    bindless_slot_readwrite_->SetAndCommit(this);
    bindless_ = true;
}

MI_NAMESPACE_END