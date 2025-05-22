/*
 * Created: 2025/5/4
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_scene.h>

#include "r_internal_common.h"
#include "r_view_common.h"
#include "renderer/mi_texture.h"

MI_NAMESPACE_BEGIN
class SkyShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(SkyShaderParameters)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(TextureCube, SkyTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Output)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(SkyShaderParameters)
    DECLARE_SHADER()
};
IMPLEMENT_RDG_GRAPHICS_SHADER(SkyShader, "shaders/renderer/Sky.hlsl", "VS_Main", "PS_Main");

void Renderer::Render_Sky(RendererView *view, RenderGraphBuilder &builder) {
    auto &lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<SkyShader>();
    auto params = builder.Allocate<SkyShader::SkyShaderParameters>();
    {
        params->Output = view->G_albedo_.Raw();
        params->Output.load_op = RHILoadOpType::kDontCare;
        params->Output.store_op = RHIStoreOpType::kStore;
        params->View = view->view_common_params_;
        params->SkyTexture = view->imported.sky_texture.Raw();
        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    }
    builder.AddPass<SkyShader>(
        {}, params,
        [shader, params](RDGPass *pass, RHICommandQueueGraphics &queue) {
            RDGCommandHelper::Draw<SkyShader>(queue, pass, shader, params, 3);
        }
    );
}


MI_NAMESPACE_END