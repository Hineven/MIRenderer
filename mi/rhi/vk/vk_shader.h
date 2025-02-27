/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_VK_SHADER_H
#define MIRENDERER_VK_SHADER_H

#include "rhi/rhi_shader.h"

MI_NAMESPACE_BEGIN

class VulkanShader : public RHIShader {
public:
    // Inherit the constructor
    using RHIShader::RHIShader;
    ~VulkanShader () override ;
protected:

    bool CompileRHI () override ;
    void ResetRHI () override ;

};

MI_NAMESPACE_END

#endif //MIRENDERER_VK_SHADER_H
