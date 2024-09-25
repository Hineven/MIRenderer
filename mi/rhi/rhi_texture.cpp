/*
 * Created: 2024/9/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_texture.h"
#include "rhi_bindless.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

RHITexture::RHITexture(
        RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
RHITextureUsageFlags usage, int mip_levels, int array_layers
): type_(type), dimensions_(dimensions), format_(format), usage_(usage),
mip_levels_(mip_levels), array_layers_(array_layers) {}

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