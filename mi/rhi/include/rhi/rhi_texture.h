/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_TEXTURE_H
#define MIRENDERER_RHI_TEXTURE_H

#include "core/pixel_format.h"

#include "rhi/rhi_resource.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_desc.h"
#include "rhi/rhi_bindlesskeeper.h"
#include "core/constants.h"

MI_NAMESPACE_BEGIN

class RHITexture : public RHIResource {
protected:
    // You can only create buffers via factory functions in the RHI instance
    FORCEINLINE RHITexture(
            RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
            RHITextureUsageFlags usage, uint32_t mip_levels = 1, uint32_t array_layers = 1
    ): RHITexture(RHITextureDesc{type, dimensions, mip_levels, array_layers, format, usage}) {}
    RHITexture(RHITextureDesc desc);
    ~RHITexture() override;
public:

    FORCEINLINE int GetMipLevels() const { return desc_.mip_levels; }
    FORCEINLINE int GetArrayLayers() const { return desc_.array_layers; }
    FORCEINLINE RHITextureType GetType() const { return desc_.type; }
    FORCEINLINE RHITextureDimensions GetDimensions() const { return desc_.dimensions; }
    FORCEINLINE PixelFormatType GetFormat() const { return desc_.format; }
    FORCEINLINE RHITextureUsageFlags GetUsage() const { return desc_.usage; }
    FORCEINLINE uint32_t GetWidth() const { return desc_.dimensions.width; }
    FORCEINLINE uint32_t GetHeight() const { return desc_.dimensions.height; }
    FORCEINLINE uint32_t GetDepth() const { return desc_.dimensions.depth; }
    FORCEINLINE RHITextureLayoutType const GetLayout_RHIThread () const { return layout_; }

    FORCEINLINE RHITextureDesc GetDesc() const { return desc_; }
    FORCEINLINE size_t GetSize () const { return size_; }

protected:
    // Actual layout. Mainly accessible to the RHI thread.
    RHITextureLayoutType layout_ {RHITextureLayoutType::kUndefined};
    RHITextureDesc desc_;

    size_t size_ {};

};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_TEXTURE_H
