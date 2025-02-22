/*
 * Created: 2024/9/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_texture.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

RHITexture::RHITexture(
        RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
RHITextureUsageFlags usage, int mip_levels, int array_layers
): type_(type), dimensions_(dimensions), format_(format), usage_(usage),
mip_levels_(mip_levels), array_layers_(array_layers) {}

RHITexture::~RHITexture() {

}

MI_NAMESPACE_END