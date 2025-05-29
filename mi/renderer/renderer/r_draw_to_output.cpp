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
public:
    struct DrawToOutputUB {
        glm::vec2 InTextureDimensions;
        glm::vec2 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_UNIFORM_BUFFER(DrawToOutputUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, InTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RENDER_TARGET(PixelFormatType::kB8G8R8A8_SRGB, Output)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawToOutputShader, "mi/renderer/shaders/DrawToOutput.hlsl", "VS_Main", "PS_Main");

void Renderer::Render_DrawToOutput(RendererView * view, RenderGraphBuilder & builder, RDGTexture *texture) {
    auto & lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<DrawToOutputShader>();
    auto params = builder.Allocate<DrawToOutputShader::ShaderParameters>();
    {
        params->UB = builder.Allocate<DrawToOutputShader::DrawToOutputUB>();
        auto dims = texture->GetDesc().dimensions;
        params->UB->InTextureDimensions = glm::vec2(dims.width, dims.height);
        params->Output = builder.Import(RHI::Get().GetBackBuffer());
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