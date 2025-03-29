/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_texture.h"
#include "vk_conversion.h"

MI_NAMESPACE_BEGIN

VulkanTexture::VulkanTexture(RHITextureDesc desc, bool imported) :
                         RHITexture(desc) {
    vk_aspect_ = GetVulkanImageAspectFlags(desc.usage);
    if(imported) {
        flags_ = flags_ | RHIResourceFlagBits::kImported;
        return;
    }

    auto device = GetVulkanRHI()->GetDevice();
    auto vma = GetVulkanRHI()->GetVmaAllocator();

    if(desc.type == RHITextureType::kCube) {
        mi_assert(desc.array_layers == 6, "Cube texture must have 6 array layers!");
    }

    // Create image
    vk::ImageCreateInfo image_create_info{
            vk::ImageCreateFlags{},
            GetVulkanImageType(desc.type),
            GetVulkanPixelFormat(desc.format),
            vk::Extent3D{static_cast<uint32_t>(desc.dimensions.width), static_cast<uint32_t>(desc.dimensions.height),
                         static_cast<uint32_t>(desc.dimensions.depth)},
            static_cast<uint32_t>(desc.mip_levels),
            static_cast<uint32_t>(desc.array_layers),
            vk::SampleCountFlagBits::e1,
            vk::ImageTiling::eOptimal,
            GetVulkanImageUsage(desc.usage),
            vk::SharingMode::eExclusive,
            0,
            nullptr,
            vk::ImageLayout::eUndefined
    };
    // Allocate memory for the image
    bool use_dedicated_allocation =
            (desc.usage & RHITextureUsageFlagBits::kDepthStencil)
            || (desc.usage & RHITextureUsageFlagBits::kRenderTarget);

    auto result = vma.createImage(image_create_info, vma::AllocationCreateInfo{
            use_dedicated_allocation
            ? vma::AllocationCreateFlagBits::eDedicatedMemory : vma::AllocationCreateFlags{},
            vma::MemoryUsage::eAutoPreferDevice
    });

    // Set the size of the image
    size_ = vma.getAllocationInfo(result.second).size;

    vk_image_layout_ = vk::ImageLayout::eUndefined;

    vk_image_ = result.first;
    allocation_ = result.second;

    mi_assert(vk_image_ && allocation_, "Failed to allocate texture!");

    CreateDefaultImageView();
}

void VulkanTexture::CreateDefaultImageView () {
    auto device = GetVulkanRHI()->GetDevice();
    // Create a default image view
    vk_default_image_view_ = device.createImageView(vk::ImageViewCreateInfo{
            vk::ImageViewCreateFlags{},
            vk_image_,
            GetVulkanImageViewType(GetType()),
            GetVulkanPixelFormat(GetFormat()),
            vk::ComponentMapping{}, // identity swizzle by default
            vk::ImageSubresourceRange{
                    GetVulkanImageAspectFlags(GetUsage()),
                    0,
                    static_cast<uint32_t>(GetMipLevels()),
                    0,
                    static_cast<uint32_t>(GetArrayLayers())
            }
    });
}

VulkanTexture::~VulkanTexture () {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroyImageView(vk_default_image_view_);
    // Imported textures should not be destroyed because they are allocated externally by the user
    if(!(GetFlags() & RHIResourceFlagBits::kImported)) {
        auto vma = GetVulkanRHI()->GetVmaAllocator();
        vma.destroyImage(vk_image_, allocation_);
        allocation_ = nullptr;
    }
    // Need to do nothing about imported resources
}

void VulkanTexture::ImportFromHandle(vk::Image image_handle, vk::ImageLayout imported_layout) {
    mi_assert(GetFlags() & RHIResourceFlagBits::kImported, "Resource must be imported to use this function!");
    vk_image_ = image_handle;
    vk_image_layout_ = imported_layout;
    vk_aspect_ = GetVulkanImageAspectFlags(GetUsage());
    allocation_ = nullptr;
    CreateDefaultImageView();
}

MI_NAMESPACE_END
