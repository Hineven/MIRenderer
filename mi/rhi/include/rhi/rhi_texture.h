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

MI_NAMESPACE_BEGIN

class RHITexture : public RHIResource {
public:
    FORCEINLINE RHITexture(
            RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
            RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1
    ): type_(type), dimensions_(dimensions), format_(format), usage_(usage), mip_levels_(mip_levels), array_layers_(array_layers) {}
    ~RHITexture() override = default;

    FORCEINLINE int GetMipLevels() const { return mip_levels_; }
    FORCEINLINE int GetArrayLayers() const { return array_layers_; }
    FORCEINLINE RHITextureType GetType() const { return type_; }
    FORCEINLINE RHITextureDimensions GetDimensions() const { return dimensions_; }
    FORCEINLINE PixelFormatType GetFormat() const { return format_; }
    FORCEINLINE RHITextureUsageFlags GetUsage() const { return usage_; }
    FORCEINLINE int GetWidth() const { return dimensions_.width; }
    FORCEINLINE int GetHeight() const { return dimensions_.height; }
    FORCEINLINE int GetDepth() const { return dimensions_.depth; }

    // Convert this texture to a bindless texture
    void ConvertToBindless () ;

protected:
    RHITextureType type_;
    RHITextureDimensions dimensions_;
    PixelFormatType format_;
    RHITextureUsageFlags usage_;
    int mip_levels_;
    int array_layers_;
};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_TEXTURE_H
