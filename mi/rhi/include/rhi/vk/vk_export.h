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
};

// Used to supply extra information to the Vulkan RHI upon creation
struct VulkanRHICreateInfo {
  	const char ** extra_instance_extensions;
    uint32_t extra_instance_extension_count;
};

MI_NAMESPACE_END
#endif //VK_PROPS_H
