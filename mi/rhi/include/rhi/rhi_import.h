/*
 * Created: 2024/9/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_IMPORT_H
#define MI_RHI_IMPORT_H

#include "core/common.h"
MI_NAMESPACE_BEGIN

// Structures used to pass external resource import details between RHI and the application

struct VulkanTextureImportDesc {
    // VkImage
    void * vk_image;
    // VkImageLayout (enum)
    uint32_t vk_image_layout;
};

MI_NAMESPACE_END
#endif //MI_RHI_IMPORT_H
