/*
 * Created: 2025/5/4
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_scene.h>

#include "r_internal_common.h"
#include "r_view_common.h"
#include "r_draw_to_output.h"
#include "renderer/mi_texture.h"

MI_NAMESPACE_BEGIN
class SkyShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(SkyShaderParameters)
        SHADER_PARAMETER_STRUCT_REF(ViewCommonShaderParameters, View)
        SHADER_PARAMETER(TextureCube, SkyTexture)
        SHADER_PARAMETER(SamplerState, Sampler)
        SHADER_USE_RENDERPASS(DrawToOutputPass, Pass)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(SkyShaderParameters)
    DECLARE_SHADER()
};
IMPLEMENT_RDG_GRAPHICS_SHADER(SkyShader, "shaders/Sky.hlsl", "VS_Main", "PS_Main");

void Renderer::Render_Sky(RendererView *view, RenderGraphBuilder &builder) {
    auto &lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<SkyShader>();
    auto pass = builder.Allocate<DrawToOutputPass>();
    auto params = builder.Allocate<SkyShader::SkyShaderParameters>();
    {
        pass->Output = view->imported.output_.Raw();
        params->Pass = pass;
        params->View = view->view_common_params_;
        params->SkyTexture = view->imported.sky_texture.Raw();
        params->Sampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    }
    builder.AddPass<SkyShader>(
        {}, params,
        [shader, params](RDGPass *pass, RHICommandQueueGraphics &queue) {
            RDGCommandHelper::Draw<SkyShader>(queue, pass, shader, params, 3);
        }
    );
}


MI_NAMESPACE_END