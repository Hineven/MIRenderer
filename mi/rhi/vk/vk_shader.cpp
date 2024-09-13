/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "vk_rhi.h"
#include "vk_shader.h"
MI_NAMESPACE_BEGIN

bool VulkanShader::CompileRHI() {
    auto device = GetVulkanRHI()->GetDevice();
    vk_shader_module_ = device.createShaderModule(
            vk::ShaderModuleCreateInfo()
            .setPCode(reinterpret_cast<const uint32_t *>(ir_.get())).setCodeSize(ir_size_)
    );
    return (bool)vk_shader_module_;
}

void VulkanShader::ResetRHI() {
    auto device = GetVulkanRHI()->GetDevice();
    if(vk_shader_module_) {
        device.destroyShaderModule(vk_shader_module_);
    }
    vk_shader_module_ = nullptr;
}

MI_NAMESPACE_END
