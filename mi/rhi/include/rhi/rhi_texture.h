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
    RHITextureDesc desc_;

    bool is_bindless_optimal_accessed_ {false};
};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_TEXTURE_H
