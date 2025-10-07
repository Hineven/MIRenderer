/*
 * Created: 2025/9/22
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <rdg/rdg_shader.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_cvar.h>
#include <renderer/mi_renderer.h>
#include "r_view_common.h"
#include "r_persistent.h"
#include "r_denoiser.h"
MI_NAMESPACE_BEGIN

static CVar<bool> CVar_UseDilatedConvolution("r.denoise_diffuse_direct_lighting.use_dilated_convolution",
    "Whether to use dilated convolution for denoising diffuse direct lighting. If false, bypass that and output prefiltered result.",
    true);

static CVar<float> CVar_DilatedConvolutionLuminanceSize("r.denoise_diffuse_direct_lighting.dilated_convolution_luminance_size",
    "Luminance similarity size for dilated convolution. Larger is more aggressive denoising but may lose details.",
    1.0f
);

static CVar<float> CVar_ConvolutionNormalDifferenceWeight("r.denoise_diffuse_direct_lighting.convolution_normal_difference_weight",
    "Weight for normal difference when computing weights in the dilated convolution. Larger, the stricter.",
    5.0f
);

static CVar<bool> CVar_DenoiseDiffuseIndirect("r.denoise_diffuse_indirect.enable",
    "Whether to denoise diffuse indirect lighting.",
    true
);

bool DenoiserPersistentData::MakeSureExists(RendererView * view, RenderGraphBuilder & builder) {
    bool flag = false;
    // Actually, we don't need to do anything. Passing null resources to shader
    // fallbacks to a default and safe behavior. Just tell the shader not to use history.
    if (!prev_lighting_history_length) {
        flag = true;
    }
    if (!prev_prefiltered_diffuse_direct_lighting) {
        flag = true;
    }
    if (!prev_denoised_diffuse_indirect_lighting) {
        flag = true;
    }
    return flag;
}

struct DenoiseDiffuseLightingUB {
    uint  Reset;
    uint  FrameIndex;
    uint  MaxHistoryLength;
    uint  MaxFastHistoryLength;
    float DepthHistoryThreshold;
    float DilatedConvolutionLuminanceSize;
    float ConvolutionNormalDifferenceWeight;
    uint32_t DenoiseDiffuseIndirect;
};

class PreFilterDiffuseLightingAndTemporalAccumulateShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(DenoiseDiffuseLightingUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Normal)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(Texture2D, PreviousDepthTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, InputDiffuseDirectRadianceTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, InputDiffuseIndirectRadianceTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, PreviousHistoryLengthTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWHistoryLengthTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, PreviousPreFilteredDiffuseDirectRadianceTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, PreviousDenoisedDiffuseIndirectRadianceTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWPreFilteredDiffuseDirectRadianceTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDenoisedDiffuseDirectRadianceTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDenoisedDiffuseIndirectRadianceTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointBorder0Sampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
    constexpr static uint32_t kTileSize = 16;
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "TILE_SIZE=" + std::to_string(kTileSize)
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(PreFilterDiffuseLightingAndTemporalAccumulateShader, "mi/renderer/shaders/DenoiseDiffuseLighting.hlsl", "PreFilterDiffuseLightingAndTemporalAccumulate");

class DilatedFilterDiffuseDirectLightingShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(DenoiseDiffuseLightingUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Normal)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(Texture2D, PreviousDepthTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, HistoryLengthTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, DilatedFilterInputRadianceTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDilatedFilterOutputFilteredRadiance)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointBorder0Sampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
    constexpr static uint32_t kTileSize = 16;
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "TILE_SIZE=" + std::to_string(kTileSize)
        };
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "DILATED_FILTER_PASS=0",
            "DILATED_FILTER_PASS=1",
            "DILATED_FILTER_PASS=2",
            "LAST_PASS"
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(DilatedFilterDiffuseDirectLightingShader, "mi/renderer/shaders/DenoiseDiffuseLighting.hlsl", "DilatedFilterDiffuseDirectLighting");

void Renderer::Render_DenoiseLighting(RendererView *view, RenderGraphBuilder &builder) {

    bool need_reset = false;
    if (!view->persistent_data_->denoiser_persistent_data_) {
        view->persistent_data_->denoiser_persistent_data_ = new DenoiserPersistentData();
    }
    need_reset |= view->persistent_data_->denoiser_persistent_data_->MakeSureExists(view, builder);

    auto history_length = builder.CreateTexture2D(
        view->film_width_, view->film_height_,
        PixelFormatType::kR8_UNORM
    );
    history_length->SetName("CurrentLightingHistoryLength");
    auto prefiltered_diffuse_direct_lighting = builder.CreateTexture2D(
        view->film_width_, view->film_height_,
        PixelFormatType::kR16G16B16A16_FLOAT
    );
    prefiltered_diffuse_direct_lighting->SetName("CurrentPreFilteredDiffuseDirectLighting");
    auto denoised_diffuse_indirect_lighting = builder.CreateTexture2D(
        view->film_width_, view->film_height_,
        PixelFormatType::kR16G16B16A16_FLOAT
    );
    denoised_diffuse_indirect_lighting->SetName("CurrentDenoisedDiffuseIndirectLighting");

    auto & lib = RDGShaderLibrary::Get();
    auto UB = builder.Allocate<DenoiseDiffuseLightingUB>();
    {
        UB->Reset = need_reset;
        UB->FrameIndex = view->persistent_data_->frame_index_;
        UB->MaxHistoryLength = 32;
        UB->MaxFastHistoryLength = 8;
        UB->DepthHistoryThreshold = 0.001f;
        UB->DilatedConvolutionLuminanceSize = glm::clamp(CVar_DilatedConvolutionLuminanceSize.Get(), 0.01f, 100.f);
        UB->ConvolutionNormalDifferenceWeight = glm::clamp(CVar_ConvolutionNormalDifferenceWeight.Get(), 0.01f, 100.f);
        UB->DenoiseDiffuseIndirect = CVar_DenoiseDiffuseIndirect.Get() ? 1 : 0;
    }
    {
        auto ini = RDGShaderInitializationInfo {};
        if (!CVar_UseDilatedConvolution.Get()) {
            ini.optional_macros.push_back("OUTPUT_DIRECTLY");
        }
        auto shader = lib.GetShader<PreFilterDiffuseLightingAndTemporalAccumulateShader>(ini);
        auto params = builder.Allocate<PreFilterDiffuseLightingAndTemporalAccumulateShader::Params>();
        params->View = view->view_common_params_;
        params->UB = UB;
        params->G_Normal = view->G_normal_.Raw();
        params->G_Depth = view->G_depth_.Raw();
        params->PreviousDepthTexture = view->persistent_data_->prev_G_depth.Raw();
        params->InputDiffuseDirectRadianceTexture = view->diffuse_direct_lighting_.Raw();
        params->InputDiffuseIndirectRadianceTexture = view->diffuse_indirect_lighting_.Raw();
        params->PreviousHistoryLengthTexture = view->persistent_data_->denoiser_persistent_data_->prev_lighting_history_length.Raw();
        params->RWHistoryLengthTexture = history_length.Raw();
        params->PreviousPreFilteredDiffuseDirectRadianceTexture = view->persistent_data_->denoiser_persistent_data_->prev_prefiltered_diffuse_direct_lighting.Raw();
        params->PreviousDenoisedDiffuseIndirectRadianceTexture = view->persistent_data_->denoiser_persistent_data_->prev_denoised_diffuse_indirect_lighting.Raw();
        params->RWPreFilteredDiffuseDirectRadianceTexture = prefiltered_diffuse_direct_lighting.Raw();
        params->RWDenoisedDiffuseIndirectRadianceTexture = denoised_diffuse_indirect_lighting.Raw();
        if (!CVar_UseDilatedConvolution.Get()) {
            // Skip dilated convolution, output directly
            params->RWDenoisedDiffuseDirectRadianceTexture = view->denoised_diffuse_direct_lighting_.Raw();
        } else {
            params->RWDenoisedDiffuseDirectRadianceTexture = nullptr;
        }
        params->PointBorder0Sampler = RHI::Get().GetGlobalSamplers().point_border_0;
        params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
        Helpers::AddComputePass<PreFilterDiffuseLightingAndTemporalAccumulateShader>(
            builder, shader, params,
            DivideAndRoundUp(view->film_width_, PreFilterDiffuseLightingAndTemporalAccumulateShader::kTileSize),
            DivideAndRoundUp(view->film_height_, PreFilterDiffuseLightingAndTemporalAccumulateShader::kTileSize)
        );

        // Output denoised diffuse indirect lighting
        view->denoised_diffuse_indirect_lighting_ = denoised_diffuse_indirect_lighting;
    }
    // Dilated convolution
    if (CVar_UseDilatedConvolution.Get()) {
        constexpr int kMaxPasses = 2;
        TRef<RDGTexture> input_texture, output_texture;
        input_texture = prefiltered_diffuse_direct_lighting;
        for (int i = 0; i < kMaxPasses; i++) {
            auto params = builder.Allocate<DilatedFilterDiffuseDirectLightingShader::Params>();
            auto ini = RDGShaderInitializationInfo {};
            ini.optional_macros.push_back("DILATED_FILTER_PASS=" + std::to_string(i));
            if (i == kMaxPasses - 1) {
                ini.optional_macros.push_back("LAST_PASS");
            }
            auto shader = lib.GetShader<DilatedFilterDiffuseDirectLightingShader>(ini);
            params->View = view->view_common_params_;
            params->UB = UB;
            params->G_Normal = view->G_normal_.Raw();
            params->G_Depth = view->G_depth_.Raw();
            params->PreviousDepthTexture = view->persistent_data_->prev_G_depth.Raw();
            params->HistoryLengthTexture = history_length.Raw();
            params->DilatedFilterInputRadianceTexture = input_texture.Raw();
            if (i != kMaxPasses - 1) {
                output_texture = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR16G16B16A16_FLOAT);
            } else {
                output_texture = view->denoised_diffuse_direct_lighting_;
            }
            params->RWDilatedFilterOutputFilteredRadiance = output_texture.Raw();
            params->PointBorder0Sampler = RHI::Get().GetGlobalSamplers().point_border_0;
            params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
            Helpers::AddComputePass<DilatedFilterDiffuseDirectLightingShader>(
                builder, shader, params,
                DivideAndRoundUp(view->film_width_, DilatedFilterDiffuseDirectLightingShader::kTileSize),
                DivideAndRoundUp(view->film_height_, DilatedFilterDiffuseDirectLightingShader::kTileSize)
            );
            // Ping pong
            input_texture = output_texture;
        }
    }
    // Finally, update persistent data
    {
        auto persistent = view->persistent_data_->denoiser_persistent_data_;
        persistent->prev_lighting_history_length = history_length;
        persistent->prev_lighting_history_length->SetName("PrevLightingHistoryLength");
        persistent->prev_lighting_history_length->SetExport();

        persistent->prev_prefiltered_diffuse_direct_lighting = prefiltered_diffuse_direct_lighting;
        persistent->prev_prefiltered_diffuse_direct_lighting->SetName("PrevPreFilteredDiffuseDirectLighting");
        persistent->prev_prefiltered_diffuse_direct_lighting->SetExport();

        persistent->prev_denoised_diffuse_indirect_lighting = denoised_diffuse_indirect_lighting;
        persistent->prev_denoised_diffuse_indirect_lighting->SetName("PrevDenoisedDiffuseIndirectLighting");
        persistent->prev_denoised_diffuse_indirect_lighting->SetExport();
    }
}


MI_NAMESPACE_END