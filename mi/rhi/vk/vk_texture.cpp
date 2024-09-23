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
                             int array_layers, bool imported):
                         RHITexture(type, dimensions, format, usage, mip_levels, array_layers) {
    vk_aspect_ = GetVulkanImageAspectFlags(usage);

    if(imported) {
        return;
    }

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
    // Allocate memory for the image
    bool use_dedicated_allocation =
            (usage & RHITextureUsageFlagBits::kDepthStencil)
            || (usage & RHITextureUsageFlagBits::kRenderTarget);

    auto result = vma.createImage(image_create_info, vma::AllocationCreateInfo{
            use_dedicated_allocation
            ? vma::AllocationCreateFlagBits::eDedicatedMemory : vma::AllocationCreateFlags{},
            vma::MemoryUsage::eAutoPreferDevice
    });

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
    printf("Destroy!\n");
    fflush(stdout);
    if(!(GetFlags() & RHIResourceFlagBits::kImported)) {
        auto device = GetVulkanRHI()->GetDevice();
        auto vma = GetVulkanRHI()->GetVmaAllocator();
        device.destroyImageView(vk_default_image_view_);
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

bool VulkanFramebuffer::CompileRHI(const RHIFramebufferDesc &desc) {
    auto device = GetVulkanRHI()->GetDevice();

    vk::FramebufferAttachmentsCreateInfo attachments_desc{};
    IVector<vk::FramebufferAttachmentImageInfo> attachment_image_infos(desc.num_attachments);
    vk::Format vk_formats[C::kRHIMaxNumFramebufferAttachments];

    for (uint32_t i = 0; i < desc.num_attachments; ++i) {
        vk_formats[i] = GetVulkanPixelFormat(desc.formats[i]);
        attachment_image_infos[i] = vk::FramebufferAttachmentImageInfo{
                vk::ImageCreateFlags{},
                IsDepthStencilPixelFormat(desc.formats[i])
            ? vk::ImageUsageFlagBits::eColorAttachment
            : vk::ImageUsageFlagBits::eDepthStencilAttachment,
                desc.width,
                desc.height,
                1, vk_formats[i]
        };
    }

    attachments_desc.setAttachmentImageInfos(attachment_image_infos);
    auto framebuffer_desc = vk::FramebufferCreateInfo{
        vk::FramebufferCreateFlagBits::eImageless,
        nullptr,
        desc.num_attachments,
        nullptr,
        desc.width,
        desc.height,
        1
    };
    framebuffer_desc.pNext = &attachments_desc;
    vk_framebuffer_ = device.createFramebuffer(framebuffer_desc);
    is_valid_ = (vk_framebuffer_ != nullptr);
    return is_valid_;
}

VulkanFramebuffer::~VulkanFramebuffer() {
    auto device = GetVulkanRHI()->GetDevice();
    if(vk_framebuffer_) {
        device.destroyFramebuffer(vk_framebuffer_);
    }
}

MI_NAMESPACE_END
