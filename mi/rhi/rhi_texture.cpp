/*
 * Created: 2024/9/18
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/rhi_texture.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

RHITexture::RHITexture(RHITextureDesc desc): desc_(desc) {}

RHITexture::~RHITexture() {

}

MI_NAMESPACE_END