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
    RHITexture(
            RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
            RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1
    );
    ~RHITexture() override;
public:

    FORCEINLINE int GetMipLevels() const { return mip_levels_; }
    FORCEINLINE int GetArrayLayers() const { return array_layers_; }
    FORCEINLINE RHITextureType GetType() const { return type_; }
    FORCEINLINE RHITextureDimensions GetDimensions() const { return dimensions_; }
    FORCEINLINE PixelFormatType GetFormat() const { return format_; }
    FORCEINLINE RHITextureUsageFlags GetUsage() const { return usage_; }
    FORCEINLINE uint32_t GetWidth() const { return dimensions_.width; }
    FORCEINLINE uint32_t GetHeight() const { return dimensions_.height; }
    FORCEINLINE uint32_t GetDepth() const { return dimensions_.depth; }
    FORCEINLINE RHITextureLayoutType const GetLayout() const { return layout_; }

    // Convert this texture to a bindless texture
    // If optimal_access is true, the texture will always be in optimal layout when accessed.
    void ConvertToBindless (bool optimal_access) ;

    // Returns true if the texture is bindless and should always be in optimal
    // layout when accessed. (And you should manually take care of layout transitions.)
    // Usually read-only atlas textures should enable this and transit to shader
    // readonly optimal.
    FORCEINLINE bool IsBindlessUseOptimalAccess () {return is_bindless_optimal_accessed_;}

    FORCEINLINE RHIBindlessSlotRef<RHITexture> GetBindlessSlotReadonly() { return bindless_slot_readonly_; }
    FORCEINLINE RHIBindlessSlotRef<RHITexture> GetBindlessSlotReadwrite() { return bindless_slot_readwrite_; }

protected:

    RHIBindlessSlotRef<RHITexture> bindless_slot_readonly_;
    RHIBindlessSlotRef<RHITexture> bindless_slot_readwrite_;

    RHITextureLayoutType layout_ {RHITextureLayoutType::kUndefined};
    RHITextureType type_;
    RHITextureDimensions dimensions_;
    PixelFormatType format_;
    RHITextureUsageFlags usage_;
    int mip_levels_;
    int array_layers_;

    bool is_bindless_optimal_accessed_ {false};
};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_TEXTURE_H
