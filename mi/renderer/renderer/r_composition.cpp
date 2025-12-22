/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "r_denoiser.h"
#include "r_diffuse_direct_lighting.h"
#include "r_diffuse_indirect_lighting.h"
#include "../include/renderer/r_geometry_buffer.h"
#include "r_volume_direct_lighting.h"
#include "r_volume_indirect_lighting.h"
#include "r_volume_grid_direct_lighting.h"
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer.h"
#include "r_view_common.h"
#include "r_volume_direct_lighting.h"
#include "rdg/rdg_helper.h"
#include "../renderer/r_persistent.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
MI_NAMESPACE_BEGIN
static CVar<float> CVar_Exposure(
    "r.output.exposure",
    "Exposure value for the final output. "
    "This is used to adjust the brightness of the final image.",
    0.0f
);

static CVar<bool> CVar_EnableAccumulation(
    "r.composition.enable_accumulation",
    "Enable accumulation for final radiance across frames",
    false
);

static CVar<bool> CVar_UseDenoisedDirectLighting(
    "r.composition.use_denoised_direct_lighting",
    "Use denoised direct lighting for the final composition",
    true
);

static CVar<bool> CVar_UseDenoisedIndirectLighting(
    "r.composition.use_denoised_indirect_lighting",
    "Use denoised indirect (diffuse & volume) lighting in final composition",
    true
);

static CVar<bool> CVar_EnableDiffuseDirect(
    "r.composition.enable_diffuse_direct",
    "Enable diffuse direct contribution in composition",
    true
);
static CVar<bool> CVar_EnableDiffuseIndirect(
    "r.composition.enable_diffuse_indirect",
    "Enable diffuse indirect contribution in composition",
    true
);
static CVar<bool> CVar_EnableVolumeDirect(
    "r.composition.enable_volume_direct",
    "Enable volume direct contribution in composition",
    true
);
static CVar<bool> CVar_EnableVolumeIndirect(
    "r.composition.enable_volume_indirect",
    "Enable volume indirect contribution in composition",
    true
);
static CVar<bool> CVar_EnableVolumeGridDirect(
    "r.composition.enable_volume_grid_direct",
    "Enable volume grid direct contribution in composition",
    false
);

class LightingCompositionShader : public RDGShader {
public:
    struct LightingCompositionUB {
        uint32_t EnableAccumulation;
        uint32_t EnableDiffuseDirect;
        uint32_t EnableDiffuseIndirect;
        uint32_t EnableVolumeDirect;
        uint32_t EnableVolumeIndirect;
        uint32_t EnableVolumeGridDirect;
        uint32_t Padding[2]; // keep 16-byte alignment
    };
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(LightingCompositionUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, DiffuseDirectLightingTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, DiffuseIndirectLightingTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, VolumeDirectLightingTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, VolumeIndirectLightingTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, VolumeGridDirectLightingTexture)
        SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Albedo)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Emission)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Transmittance)
        SHADER_RESOURCE_PARAMETER(Texture2D, HistoryRadiance)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWRadiance)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWShadedRadianceWithoutEmission)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWShadedVolumeRadiance)
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
        UB->EnableAccumulation     = CVar_EnableAccumulation.Get()     ? 1 : 0;
        UB->EnableDiffuseDirect    = CVar_EnableDiffuseDirect.Get()    ? 1 : 0;
        UB->EnableDiffuseIndirect  = CVar_EnableDiffuseIndirect.Get()  ? 1 : 0;
        UB->EnableVolumeDirect     = CVar_EnableVolumeDirect.Get()     ? 1 : 0;
        UB->EnableVolumeIndirect   = CVar_EnableVolumeIndirect.Get()   ? 1 : 0;
        UB->EnableVolumeGridDirect = CVar_EnableVolumeGridDirect.Get() ? 1 : 0;
        UB->Padding[0] = UB->Padding[1] = 0;
    }
    params->UB = UB;
    if (!CVar_UseDenoisedDirectLighting.Get()) {
        params->DiffuseDirectLightingTexture = view->diffuse_direct_lighting_->radiance.Raw();
        params->VolumeDirectLightingTexture = view->volume_direct_lighting_->radiance.Raw();
        params->VolumeGridDirectLightingTexture = view->volume_grid_direct_lighting_->radiance.Raw();
    } else {
        params->DiffuseDirectLightingTexture = view->denoiser_->denoised_diffuse_direct_lighting.Raw();
        params->VolumeDirectLightingTexture = view->denoiser_->denoised_volume_direct_lighting.Raw();
        params->VolumeGridDirectLightingTexture = view->volume_grid_direct_lighting_->radiance.Raw();
    }
    if (CVar_UseDenoisedIndirectLighting.Get()) {
        params->DiffuseIndirectLightingTexture = view->denoiser_->denoised_diffuse_indirect_lighting.Raw();
        params->VolumeIndirectLightingTexture = view->denoiser_->denoised_volume_indirect_lighting.Raw();
    } else {
        params->DiffuseIndirectLightingTexture = view->diffuse_indirect_lighting_->radiance.Raw();
        params->VolumeIndirectLightingTexture = view->volume_indirect_lighting_->radiance.Raw();
    }
    if (view->scene_->GetSkyTexture()) {
        params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
    } else {
        params->EnvironmentMap = nullptr;
    }
    params->G_Albedo = view->g_buffer_->G_albedo_.Raw();
    params->G_Emission = view->g_buffer_->G_emission_.Raw();
    params->G_Transmittance = view->g_buffer_->G_transmittance_.Raw();
    params->HistoryRadiance = view->persistent_data_->prev_radiance_.Raw();
    params->RWRadiance = view->radiance_.Raw();
    params->RWShadedRadianceWithoutEmission = view->shaded_radiance_no_emission_.Raw();
    params->RWShadedVolumeRadiance = view->shaded_volume_radiance_.Raw();
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
        uint  MappingType;
    };
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_UNIFORM_BUFFER(DrawToOutputUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, InTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RENDER_TARGET(PixelFormatType::kB8G8R8A8_SRGB, Output, {RHIBlendOpType::kBlendAdd, RHIBlendFactorType::kSrcAlpha, RHIBlendFactorType::kOneMinusSrcAlpha})
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawToOutputShader, "mi/renderer/shaders/DrawToOutput.hlsl", "VS_Main", "PS_Main");

void Renderer::Render_DrawToOutput(
    [[maybe_unused]] RendererView * view, RenderGraphBuilder & builder,
    RDGTexture *texture, DrawToOutputMappingType mapping_type
) {
    if (!texture) {
        return;
    }
    auto & lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<DrawToOutputShader>();
    auto params = builder.Allocate<DrawToOutputShader::ShaderParameters>();
    {
        params->UB = builder.Allocate<DrawToOutputShader::DrawToOutputUB>();
        auto dims = texture->GetDesc().dimensions;
        params->UB->InTextureDimensions = glm::vec2(dims.width, dims.height);
        params->UB->Exposure = CVar_Exposure.Get();
        params->UB->MappingType = static_cast<uint>(mapping_type);
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