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

class RHIFramebuffer : public RHIResource {
protected:
    inline RHIFramebuffer(const RHIFramebufferDesc & desc) {
        width_ = desc.width;
        height_ = desc.height;
        num_attachments_ = desc.num_attachments;
        for (uint32_t i = 0; i < num_attachments_; i++) {
            formats_[i] = desc.formats[i];
        }
    }
    virtual ~RHIFramebuffer() override;
public:
    FORCEINLINE uint32_t GetWidth()  const { return width_; }
    FORCEINLINE uint32_t GetHeight() const { return height_; }
    FORCEINLINE uint32_t GetNumAttachments() const { return num_attachments_; }
    FORCEINLINE PixelFormatType GetAttachmentFormat(uint32_t index) const { return formats_[index]; }
    FORCEINLINE bool IsValid() const { return is_valid_; }
    bool Compile(const RHIFramebufferDesc & desc) {
        if (is_valid_) {
            return true;
        }
        is_valid_ = CompileRHI(desc);
        return is_valid_;
    }

    FORCEINLINE RHITexture * GetAttachment(uint32_t index) const {
        return attachments_[index].Raw();
    }
    FORCEINLINE void SetAttachment(uint32_t index, RHITextureRef texture) {
        if(texture) {
            mi_assert(texture->GetFormat() == formats_[index], "Attachment format mismatch");
            mi_assert(texture->GetWidth() == width_ && texture->GetHeight() == height_, "Attachment size mismatch");
            if(IsDepthStencilPixelFormat(formats_[index])) {
                mi_assert(texture->GetUsage() & RHITextureUsageFlagBits::kDepthStencil, "The texture must have depth stencil usage.");
            } else {
                mi_assert(texture->GetUsage() & RHITextureUsageFlagBits::kRenderTarget, "The texture must have render target usage.");
            }
        }
        attachments_[index] = texture;
    }
    FORCEINLINE void ClearAttachments() {
        for(auto & attachment : attachments_) {
            attachment.SafeRelease();
        }
    }
protected:

    virtual bool CompileRHI(const RHIFramebufferDesc & desc) = 0;

    bool is_valid_ {false};
    uint32_t width_, height_;
    uint32_t num_attachments_ {};
    PixelFormatType formats_[C::kRHIMaxNumFramebufferAttachments];

    RHITextureRef attachments_[C::kRHIMaxNumFramebufferAttachments];
};

MI_NAMESPACE_END

#endif //MIRENDERER_RHI_TEXTURE_H
