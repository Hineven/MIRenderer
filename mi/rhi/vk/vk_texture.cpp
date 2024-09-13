/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_texture.h"
#include "vk_conversion.h"

MI_NAMESPACE_BEGIN

VulkanTexture::VulkanTexture(RHITextureType type, RHITextureDimensions dimensions,
                             PixelFormatType format, RHITextureUsageFlags usage, int mip_levels,
                             int array_layers) : RHITexture(type, dimensions, format, usage, mip_levels,
                                                            array_layers) {
    auto device = GetVulkanRHI()->GetDevice();
    auto vma = GetVulkanRHI()->GetVmaAllocator();

    if(type == RHITextureType::kCube) {
        mi_assert(array_layers == 6, "Cube texture must have 6 array layers!");
    }

    // Create image
    vk::ImageCreateInfo image_create_info{
            vk::ImageCreateFlags{},
            GetVulkanImageType(type),
            GetVulkanPixelFormat(format),
            vk::Extent3D{static_cast<uint32_t>(dimensions.width), static_cast<uint32_t>(dimensions.height),
                         static_cast<uint32_t>(dimensions.depth)},
            static_cast<uint32_t>(mip_levels),
            static_cast<uint32_t>(array_layers),
            vk::SampleCountFlagBits::e1,
            vk::ImageTiling::eOptimal,
            GetVulkanImageUsage(usage),
            vk::SharingMode::eExclusive,
            0,
            nullptr,
            vk::ImageLayout::eUndefined
    };

    vk_image_layout_ = vk::ImageLayout::eUndefined;

    // Allocate memory for the image
    bool use_dedicated_allocation =
            (usage & RHITextureUsageFlagBits::kDepth)
        || (usage & RHITextureUsageFlagBits::kRenderTarget);
    allocation_ = vma.allocateMemoryForImage(vk_image_, vma::AllocationCreateInfo{
            use_dedicated_allocation
            ? vma::AllocationCreateFlagBits::eDedicatedMemory : vma::AllocationCreateFlags{},
            vma::MemoryUsage::eAutoPreferDevice
        });
    // Create a default image view
    vk_default_image_view_ = device.createImageView(vk::ImageViewCreateInfo{
            vk::ImageViewCreateFlags{},
            vk_image_,
            GetVulkanImageViewType(type),
            GetVulkanPixelFormat(format),
            vk::ComponentMapping{}, // identity swizzle
            vk::ImageSubresourceRange{
                    GetVulkanImageAspectFlags(usage),
                    0,
                    static_cast<uint32_t>(mip_levels),
                    0,
                    static_cast<uint32_t>(array_layers)
            }
    });
    vk_aspect_ = GetVulkanImageAspectFlags(usage);
}

VulkanTexture::~VulkanTexture () {
    auto device = GetVulkanRHI()->GetDevice();
    auto vma = GetVulkanRHI()->GetVmaAllocator();
    device.destroyImageView(vk_default_image_view_);
    vma.freeMemory(allocation_);
    allocation_ = {};
}

MI_NAMESPACE_END
