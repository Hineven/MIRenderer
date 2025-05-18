/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "r_draw_to_output.h"

MI_NAMESPACE_BEGIN

class DrawToOutputShader : public RDGShader {
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_PARAMETER(Texture2D, InTexture)
        SHADER_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_USE_RENDERPASS(DrawToOutputPass, Pass)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    DECLARE_SHADER()
public:

};

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawToOutputShader, "shaders/renderer/DrawToOutput.hlsl", "VS_Main", "PS_Main");

void Renderer::Render_DrawToOutput(RendererView * view, RenderGraphBuilder & builder, RDGTexture *texture) {
    auto & lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<DrawToOutputShader>();
    auto pass = builder.Allocate<DrawToOutputPass>();
    auto params = builder.Allocate<DrawToOutputShader::ShaderParameters>();
    {
        pass->Output = view->imported.output_.Raw();
        params->Pass = pass;
        params->InTexture = texture;
        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    }
    builder.AddPass<DrawToOutputShader>(
        RDGPassFlagBits::kNeverCull, params,
        [shader, params](RDGPass * pass, RHICommandQueueGraphics & queue) {
            RDGCommandHelper::Draw<DrawToOutputShader>(queue, pass, shader, params, 3);
        }
    );
}

MI_NAMESPACE_END