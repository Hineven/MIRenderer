/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_VK_TEXTURE_H
#define MIRENDERERDEV_VK_TEXTURE_H

#include "rhi/rhi_texture.h"
#include "vk_rhi.h"
#include "core/infra.h"

MI_NAMESPACE_BEGIN

class VulkanTexture : public RHITexture {
public:
    VulkanTexture(
            RHITextureType type, RHITextureDimensions dimensions, PixelFormatType format,
            RHITextureUsageFlags usage, int mip_levels = 1, int array_layers = 1, bool imported = false
    ) ;
    ~VulkanTexture() override;

    FORCEINLINE vk::Image GetImage() const { return vk_image_; }

    FORCEINLINE vk::ImageLayout GetImageLayout () const {return vk_image_layout_;}

    // Manually create a barrier. Usually used for bindless texture atlas, whose access is not
    // maintained by us.
    FORCEINLINE void Barrier (
        vk::CommandBuffer cmd, vk::ImageLayout dst_layout,
        vk::PipelineStageFlags src_stage, vk::PipelineStageFlags dst_stage,
        vk::AccessFlags src_access, vk::AccessFlags dst_access
    ) {
        vk::ImageMemoryBarrier barrier {};
        barrier.image = vk_image_;
        barrier.oldLayout = vk_image_layout_;
        barrier.newLayout = dst_layout;
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.subresourceRange.aspectMask = vk_aspect_;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mip_levels_;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = array_layers_;
        cmd.pipelineBarrier(src_stage, dst_stage, {}, nullptr, nullptr, barrier);
        using_stages = dst_stage;
        using_accesses = dst_access;
        vk_image_layout_ = dst_layout;
    }

    // Perform a layout transition if the current layout is *suboptimal* for 'use_layout'. If there's a
    // memory barrier necessary, it will be inserted at the same time.
    // Will not perform a layout transition if the current layout is suboptimal (i.e. general layout)
    FORCEINLINE void UseOptimalLayout (
            vk::CommandBuffer cmd, vk::ImageLayout use_layout,
            vk::PipelineStageFlags use_stage = vk::PipelineStageFlagBits::eAllCommands,
            vk::AccessFlags use_access = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite) {
        // Check for the possibility to omit the barrier
        // R-R with no layout transition, the only case that doesn't need a barrier
        if(!(use_access & vk::AccessFlagBits::eMemoryWrite) && !(using_accesses & vk::AccessFlagBits::eMemoryWrite)) {
            if(vk_image_layout_ == use_layout) {
                // No layout transition needed
                using_accesses |= use_access;
                using_stages   |= use_stage;
                return;
            }
        }
        mi_assert(!(GetFlags() & RHIResourceFlagBits::kImported), "Imported resources can not be transitioned.");
        vk::ImageMemoryBarrier barrier {};
        barrier.image = vk_image_;
        barrier.oldLayout = vk_image_layout_;
        barrier.newLayout = use_layout;
        barrier.srcAccessMask = using_accesses;
        barrier.dstAccessMask = use_access;
        barrier.subresourceRange.aspectMask = vk_aspect_;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mip_levels_;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = array_layers_;
        cmd.pipelineBarrier(using_stages, use_stage, {}, nullptr, nullptr, barrier);
        using_stages = use_stage;
        using_accesses = use_access;
        vk_image_layout_ = use_layout;
    }

    // Perform a layout transition if the current layout is *incompatible* for 'use_layout'. If there's a
    // memory barrier necessary, it will be inserted at the same time.
    // Will not perform a layout transition if the current layout is suboptimal (i.e. general layout)
    FORCEINLINE void Use (
            vk::CommandBuffer cmd, vk::ImageLayout use_layout,
            vk::PipelineStageFlags use_stage = vk::PipelineStageFlagBits::eAllCommands,
            vk::AccessFlags use_access = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite) {
        if(vk_image_layout_ == vk::ImageLayout::eGeneral) use_layout = vk::ImageLayout::eGeneral;
        UseOptimalLayout(cmd, use_layout, use_stage, use_access);
    }

    FORCEINLINE vk::ImageView GetImageView () {
        return vk_default_image_view_;
    }
    
    void ImportFromHandle (vk::Image image_handle, vk::ImageLayout imported_layout) ;

    friend class VulkanRHI;
protected:

    void CreateDefaultImageView () ;

    vk::Image vk_image_ {};
    vk::ImageLayout vk_image_layout_ {};
    vk::ImageView vk_default_image_view_;
    vk::ImageAspectFlags vk_aspect_ {};
    vma::Allocation allocation_ {};

    // Submitted command stages accessing this texture
    vk::PipelineStageFlags using_stages {};
    // Submitted command accesses on this texture
    vk::AccessFlags using_accesses {};
};

MI_NAMESPACE_END

#endif //MIRENDERERDEV_VK_TEXTURE_H
