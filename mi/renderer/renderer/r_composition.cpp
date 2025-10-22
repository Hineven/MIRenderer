/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer.h"
#include "r_view_common.h"
#include "rdg/rdg_helper.h"
#include "../renderer/r_persistent.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
MI_NAMESPACE_BEGIN
    static CVar<float> CVar_Exposure(
    "r.exposure",
    "Exposure value for the final output. "
    "This is used to adjust the brightness of the final image.",
    0.0f
);

static CVar<bool> CVar_EnableAccumulation(
    "r.enable_accumulation",
    "Enable accumulation for final radiance across frames",
    false
);

static CVar<bool> CVar_UseDenoisedDirectLighting(
    "r.use_denoised_direct_lighting",
    "Use denoised direct lighting for the final composition",
    true
);



class LightingCompositionShader : public RDGShader {
public:
    struct LightingCompositionUB {
        uint32_t EnableAccumulation;
        uint32_t Padding[3]; // Padding to make it 16 bytes aligned
    };
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(LightingCompositionUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, DiffuseDirectLightingTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, VolumeDirectLightingTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, DiffuseIndirectLightingTexture)
        SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Albedo)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Emission)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Transmittance)
        SHADER_RESOURCE_PARAMETER(Texture2D, HistoryRadiance)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWRadiance)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWShadedRadianceWithoutEmission)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
    constexpr static uint32_t kTileSize = 16; // 16x16 tiles
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "TILE_SIZE=" + std::to_string(kTileSize)
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(LightingCompositionShader, "mi/renderer/shaders/LightingComposition.hlsl", "LightingComposition");

void Renderer::Render_LightingComposition(RendererView *view, RenderGraphBuilder &builder) {

    auto & lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<LightingCompositionShader>();
    auto params = builder.Allocate<LightingCompositionShader::ShaderParameters>();
    params->View = view->view_common_params_;
    auto UB = builder.Allocate<LightingCompositionShader::LightingCompositionUB>();
    {
        UB->EnableAccumulation = CVar_EnableAccumulation.Get() ? 1 : 0;
    }
    params->UB = UB;
    if (!CVar_UseDenoisedDirectLighting.Get()) {
        params->DiffuseDirectLightingTexture = view->diffuse_direct_lighting_.Raw();
    } else {
        params->DiffuseDirectLightingTexture = view->denoised_diffuse_direct_lighting_.Raw();
    }
    params->VolumeDirectLightingTexture = view->volume_direct_lighting_.Raw();
    params->DiffuseIndirectLightingTexture = view->denoised_diffuse_indirect_lighting_.Raw();
    if (view->scene_->GetSkyTexture()) {
        params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
    } else {
        params->EnvironmentMap = nullptr;
    }
    params->G_Albedo = view->G_albedo_.Raw();
    params->G_Emission = view->G_emission_.Raw();
    params->G_Transmittance = view->G_transmittance_.Raw();
    params->HistoryRadiance = view->persistent_data_->prev_radiance_.Raw();
    params->RWRadiance = view->radiance_.Raw();
    params->RWShadedRadianceWithoutEmission = view->shaded_radiance_no_emission_.Raw();
    params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    auto groups_x = DivideAndRoundUp(view->film_width_, LightingCompositionShader::kTileSize);
    auto groups_y = DivideAndRoundUp(view->film_height_, LightingCompositionShader::kTileSize);
    Helpers::AddComputePass(builder, shader, params, groups_x, groups_y, 1, RDGPassFlagBits::kNeverCull);
}


class DrawToOutputShader : public RDGShader {
public:
    struct DrawToOutputUB {
        glm::vec2 InTextureDimensions;
        float Exposure;
        float Padding;
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

void Renderer::Render_DrawToOutput(
    [[maybe_unused]] RendererView * view, RenderGraphBuilder & builder,
    RDGTexture *texture
) {
    auto & lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<DrawToOutputShader>();
    auto params = builder.Allocate<DrawToOutputShader::ShaderParameters>();
    {
        params->UB = builder.Allocate<DrawToOutputShader::DrawToOutputUB>();
        auto dims = texture->GetDesc().dimensions;
        params->UB->InTextureDimensions = glm::vec2(dims.width, dims.height);
        params->UB->Exposure = CVar_Exposure.Get();
        params->Output = builder.Import(RHI::Get().GetBackBuffer());
        params->InTexture = texture;
        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    }
    builder.AddPass<DrawToOutputShader>(
        {}, shader, params,
        [shader, params](RDGPass * pass, RHICommandQueueGraphics & queue) {
            RDGCommandHelper::Draw<DrawToOutputShader>(queue, pass, shader, params, 3);
        }
    );
}

MI_NAMESPACE_END