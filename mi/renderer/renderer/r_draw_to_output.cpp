/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "renderer/mi_renderer.h"
MI_NAMESPACE_BEGIN

class DrawToOutputShader : public RDGShader {
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_RESOURCE_PARAMETER(Texture2D, InTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Output)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawToOutputShader, "shaders/renderer/DrawToOutput.hlsl", "VS_Main", "PS_Main");

void Renderer::Render_DrawToOutput(RendererView * view, RenderGraphBuilder & builder, RDGTexture *texture) {
    auto & lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<DrawToOutputShader>();
    auto params = builder.Allocate<DrawToOutputShader::ShaderParameters>();
    {
        params->Output = view->imported.output_.Raw();
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