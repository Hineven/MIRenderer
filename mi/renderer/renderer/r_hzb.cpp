/*
 * Created: 2025/7/24
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer.h"
#include "r_view_common.h"
MI_NAMESPACE_BEGIN

class ComputeHiZBufferShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(Texture2D, InDepthBuffer)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWInHiZBuffer)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWOutHiZBuffer)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointClampSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
    constexpr static uint32_t kTileSize = 16; // 16x16 tiles
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"TILE_SIZE=" + std::to_string(kTileSize)};
    }
    static std::vector<std::string> GetShaderOptionalMacros () {
        return {"DEPTH_AS_INPUT"};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(ComputeHiZBufferShader, "mi/renderer/shaders/HiZBuffer.hlsl", "ComputeHiZBuffer");

void Renderer::Render_ComputeHiZBuffer(
    [[maybe_unused]] RendererView * view, RenderGraphBuilder & builder
) {
    auto & lib = RDGShaderLibrary::Get();
    uint32_t max_resolution = std::max(
        view->film_width_, view->film_height_
    );
    uint32_t hiz_levels = 0;
    while ((1u << hiz_levels) < max_resolution) hiz_levels ++;
    uint32_t hiz_dimensions = 1 << (hiz_levels - 1);
    view->hzb_ = RDGTexture::Create(
        RHITextureDesc {
            RHITextureType::k2D, RHITextureDimensions {hiz_dimensions, hiz_dimensions, 1u},
            hiz_levels, 1u, PixelFormatType::kR32_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource
        }
    );
    view->hzb_->SetName("HiZBuffer");
    for (uint32_t level = 0; level < hiz_levels; level++) {
        auto ini = RDGShaderInitializationInfo{};
        if (level == 0) ini.optional_macros.push_back("DEPTH_AS_INPUT");
        auto shader = lib.GetShader<ComputeHiZBufferShader>();
        auto params = builder.Allocate<ComputeHiZBufferShader::ShaderParameters>();
        params->PointClampSampler = RHI::Get().GetGlobalSamplers().point_clamp;
        params->InDepthBuffer = view->G_depth_.Raw();
        params->RWInHiZBuffer = level == 0 ? view->G_depth_.Raw() : view->hzb_.Raw();
        params->RWInHiZBuffer.mip_level = level == 0 ? 0 : (level - 1);
        params->RWOutHiZBuffer = view->hzb_.Raw();
        params->RWOutHiZBuffer.mip_level = level;
        auto groups = glm::uvec2(
            DivideAndRoundUp(1 << (hiz_levels - level - 1), ComputeHiZBufferShader::kTileSize),
            DivideAndRoundUp(1 << (hiz_levels - level - 1), ComputeHiZBufferShader::kTileSize)
        );
        builder.AddPass<ComputeHiZBufferShader>(
            {}, shader, params,
            [shader, params, groups](RDGPass * pass, RHICommandQueueGraphics & queue) {
                RDGCommandHelper::Dispatch<ComputeHiZBufferShader>(queue, pass, shader, params, groups.x, groups.y);
            }
        )->SetName("ComputeHiZBuffer Level " + std::to_string(level));
    }
}

MI_NAMESPACE_END
