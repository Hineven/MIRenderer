/*
 * Created: 2025/3/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RHI_VK_PROPS_H
#define RHI_VK_PROPS_H
#include <vulkan/vulkan.hpp>
#include "rhi/rhi_fwd.h"
MI_NAMESPACE_BEGIN

// Structures used to pass external resource import details between RHI and the application
struct VulkanTextureImportDesc {
    vk::Image vk_image;
    // VkImageLayout (enum)
    vk::ImageLayout vk_image_layout;
};

// Used to export Vulkan underlying resources to the application
struct VulkanRHIHandles {
    vk::Instance instance;
    vk::Device device;
    vk::PhysicalDevice physical_device;
    vk::Queue graphics_queue;
    uint32_t graphics_queue_family_index {};
};

// Used to supply extra information to the Vulkan RHI upon creation
struct VulkanRHICreateInfo {
  	const char ** extra_instance_extensions;
    uint32_t extra_instance_extension_count;
    const char ** extra_device_extensions;
    uint32_t extra_device_extension_count;
};

// Native Vulkan texture info for external libraries (e.g. NGX / DLSS)
struct VulkanTextureNativeInfo {
    vk::Image image;
    vk::ImageView image_view;
    vk::Format format;
    vk::ImageSubresourceRange subresource_range;
    uint32_t width;
    uint32_t height;
};

// Query native Vulkan handles from an RHITexture.
// Returns false if the texture is not backed by Vulkan or arguments are invalid.
bool RHIGetVulkanTextureInfo(class RHITexture* texture, VulkanTextureNativeInfo* out);

MI_NAMESPACE_END
#endif //VK_PROPS_H
