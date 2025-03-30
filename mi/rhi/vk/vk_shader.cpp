/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_rhi.h"
#include "vk_shader.h"
MI_NAMESPACE_BEGIN

VulkanShader::~VulkanShader () {
    ResetRHI();
}

void *VulkanShader::GetAPIHandle() const {
    MI_WARN("VulkanShader::GetAPIHandle() will return nullptr."
            "VkShaderModule is only created when assembling pipelines.");
    return (void*)nullptr;
}


bool VulkanShader::CompileRHI() {
    // Nah
    return true;
}
void VulkanShader::ResetRHI() {
    // Nah
}

MI_NAMESPACE_END
