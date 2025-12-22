/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_VK_RHI_EXPORT_H
#define MIRENDERER_VK_RHI_EXPORT_H

#include "rhi/rhi_common.h"
MI_NAMESPACE_BEGIN

class VulkanRHI;
class VulkanRHICommandExecutor;
struct VulkanRHICreateInfo;
// Instantiate a VulkanRHI instance and return
VulkanRHI * CreateVulkanRHI (const VulkanRHICreateInfo * extra);

MI_NAMESPACE_END

#endif //MIRENDERER_VK_RHI_EXPORT_H
