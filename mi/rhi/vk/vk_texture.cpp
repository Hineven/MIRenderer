/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "vk_texture.h"
#include "vk_conversion.h"

MI_NAMESPACE_BEGIN

VulkanTexture::VulkanTexture(RHITextureDesc desc, bool imported) :
                         RHITexture(desc) {

    // Do some validation
    if (desc.type == RHITextureType::k2D) {
        assert(desc.dimensions.depth == 1);
        assert(desc.array_layers == 1);
    }
    if (desc.type == RHITextureType::k2DArray) assert(desc.dimensions.depth == 1);
    if (desc.type == RHITextureType::k3D || desc.type == RHITextureType::k3DArray) assert(false && "Unimplemented");

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

    vk::ImageCreateFlags flags;
    if (desc.type == RHITextureType::kCube) {
        flags = vk::ImageCreateFlagBits::eCubeCompatible;
    } else if (desc.type == RHITextureType::k2DArray) {

    } else if (desc.type == RHITextureType::k3D) {

    } else if (desc.type == RHITextureType::k3DArray) {
        assert(false);
    }
    // Create image
    vk::ImageCreateInfo image_create_info{
            flags,
            GetVulkanImageType(desc.type),
            GetVulkanPixelFormat(desc.format),
            vk::Extent3D{desc.dimensions.width, desc.dimensions.height,
                         desc.dimensions.depth},
            desc.mip_levels,
            desc.array_layers,
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

    if (GetName() && result.second) {
        vma.setAllocationName(result.second, GetName());
    }

    vk_image_layout_ = vk::ImageLayout::eUndefined;

    vk_image_ = result.first;
    allocation_ = result.second;

    mi_assert(vk_image_ && allocation_, "Failed to allocate texture!");

    CreateDefaultImageViews();
}

void VulkanTexture::CreateDefaultImageViews () {
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
                    GetMipLevels(),
                    0,
                    GetArrayLayers()
            }
    });
    // If the image is layered, create a view for each layer
    if (GetArrayLayers() > 1 || GetMipLevels() > 1) {
        assert(vk_layer_image_views_.empty());
        for (int j = 0; j < (int)GetMipLevels(); j++) {
            for (int i = 0; i < (int)GetArrayLayers(); i++) {
                vk_layer_image_views_.push_back(
                    device.createImageView(vk::ImageViewCreateInfo{
                    vk::ImageViewCreateFlags{},
                    vk_image_,
                    vk::ImageViewType::e2D,
                    GetVulkanPixelFormat(GetFormat()),
                    vk::ComponentMapping{}, // identity swizzle by default
                    vk::ImageSubresourceRange{
                            GetVulkanImageAspectFlags(GetUsage()),
                            (uint32_t)j,
                            1,
                            (uint32_t) i,
                            1
                        }
                    }
                ));
            }
        }
    }
}

VulkanTexture::~VulkanTexture () {
    auto device = GetVulkanRHI()->GetDevice();
    device.destroyImageView(vk_default_image_view_);
    for (auto e : vk_layer_image_views_) {
        device.destroyImageView(e);
    }
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
    CreateDefaultImageViews();
}

void *VulkanTexture::GetAPIHandle() const {
    return (void*)vk_image_;
}

void VulkanTexture::SetName(const std::string &name) {
    RHITexture::SetName(name);
#ifndef NDEBUG
    GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT {
            vk::ObjectType::eImage,
            reinterpret_cast<uint64_t>((VkImage)vk_image_),
            name.c_str()
        }
    );
    GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT(
        vk::DebugUtilsObjectNameInfoEXT {
            vk::ObjectType::eImageView,
            reinterpret_cast<uint64_t>((VkImageView)vk_default_image_view_),
            name.c_str()
        }
    );
    for (const auto& [i, e] : std::views::enumerate(vk_layer_image_views_)) {
        std::string layer_name = name + "_layer_" + std::to_string(i % GetArrayLayers()) + "_mip_" + std::to_string(i / GetArrayLayers());
        GetVulkanRHI()->GetDevice().setDebugUtilsObjectNameEXT(
            vk::DebugUtilsObjectNameInfoEXT {
                vk::ObjectType::eImageView,
                reinterpret_cast<uint64_t>((VkImageView)e),
                layer_name.c_str()
            }
        );
    }
    if (allocation_) {
        auto & vma = GetVulkanRHI()->GetVmaAllocator();
        vma.setAllocationName(allocation_, GetName());
    }
#endif
}



MI_NAMESPACE_END
