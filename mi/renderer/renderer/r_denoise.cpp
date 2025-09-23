/*
 * Created: 2025/9/22
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

struct DenoiseDiffuseDirectLightingUB {
    float RotationAngle;
    uint  FrameIndex;
    uint  MaxHistoryLength;
    uint  MaxFastHistoryLength;
    float DepthHistoryThreshold;
    float DilatedConvolutionLuminanceSize;
    float ConvolutionNormalDifferenceWeight;
    uint32_t Padding;
};

class PreFilterDirectLightingAndTemporalAccumulateShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(DenoiseDiffuseDirectLightingUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Normal)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(Texture2D, HistoryDepthTexture)
        SHADER_RESOURCE_PARAMETER(Texture2D, OriginalRadianceTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWHistoryLengthTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWHistoryRadianceTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWPreFilteredRadianceTexture)
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

IMPLEMENT_RDG_COMPUTE_SHADER(PreFilterDirectLightingAndTemporalAccumulateShader, "mi/renderer/shaders/DenoiseDiffuseDirectLighting.hlsl", "PreFilterDirectLightingAndTemporalAccumulate");

class DilatedFilterDirectLightingShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(DenoiseDiffuseDirectLightingUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Normal)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(Texture2D, HistoryDepthTexture)
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

IMPLEMENT_RDG_COMPUTE_SHADER(DilatedFilterDirectLightingShader, "mi/renderer/shaders/DenoiseDiffuseDirectLighting.hlsl", "DilatedFilterDirectLighting");

void Renderer::Render_DenoiseLighting(RendererView *view, RenderGraphBuilder &builder) {
    auto & lib = RDGShaderLibrary::Get();
    auto UB = builder.Allocate<DenoiseDiffuseDirectLightingUB>();
    {
        UB->RotationAngle = 2.408f * float(view->persistent_data_->frame_index_);
        UB->FrameIndex = view->persistent_data_->frame_index_;
        UB->MaxHistoryLength = 32;
        UB->MaxFastHistoryLength = 8;
        UB->DepthHistoryThreshold = 0.001f;
        UB->DilatedConvolutionLuminanceSize = glm::clamp(CVar_DilatedConvolutionLuminanceSize.Get(), 0.01f, 100.f);
        UB->ConvolutionNormalDifferenceWeight = glm::clamp(CVar_ConvolutionNormalDifferenceWeight.Get(), 0.01f, 100.f);
        UB->Padding = {};
    }
    auto history_length = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR8_UNORM);
    auto pre_filtered_diffuse_direct_lighting_ = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR16G16B16A16_FLOAT);
    {
        auto ini = RDGShaderInitializationInfo {};
        if (!CVar_UseDilatedConvolution.Get()) {
            ini.optional_macros.push_back("OUTPUT_DIRECTLY");
        }
        auto shader = lib.GetShader<PreFilterDirectLightingAndTemporalAccumulateShader>(ini);
        auto params = builder.Allocate<PreFilterDirectLightingAndTemporalAccumulateShader::Params>();
        params->View = view->view_common_params_;
        params->UB = UB;
        params->G_Normal = view->G_normal_.Raw();
        params->G_Depth = view->G_depth_.Raw();
        params->HistoryDepthTexture = view->persistent_data_->prev_G_depth.Raw();
        params->OriginalRadianceTexture = view->diffuse_direct_lighting_.Raw();
        params->RWHistoryLengthTexture = history_length.Raw();
        params->RWHistoryRadianceTexture = view->persistent_data_->prev_denoised_diffuse_direct_lighting.Raw();
        if (!CVar_UseDilatedConvolution.Get()) {
            // Skip dilated convolution, output directly
            params->RWPreFilteredRadianceTexture = view->denoised_diffuse_direct_lighting_.Raw();
        } else {
            params->RWPreFilteredRadianceTexture = pre_filtered_diffuse_direct_lighting_.Raw();
        }
        params->PointBorder0Sampler = RHI::Get().GetGlobalSamplers().point_border_0;
        params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
        Helpers::AddComputePass<PreFilterDirectLightingAndTemporalAccumulateShader>(
            builder, shader, params,
            DivideAndRoundUp(view->film_width_, PreFilterDirectLightingAndTemporalAccumulateShader::kTileSize),
            DivideAndRoundUp(view->film_height_, PreFilterDirectLightingAndTemporalAccumulateShader::kTileSize)
        );
    }
    // Dilated convolution
    if (CVar_UseDilatedConvolution.Get()) {
        constexpr int kMaxPasses = 2;
        for (int i = 0; i < kMaxPasses; i++) {
            auto params = builder.Allocate<DilatedFilterDirectLightingShader::Params>();
            auto ini = RDGShaderInitializationInfo {};
            ini.optional_macros.push_back("DILATED_FILTER_PASS=" + std::to_string(i));
            if (i == kMaxPasses - 1) {
                ini.optional_macros.push_back("LAST_PASS");
            }
            auto shader = lib.GetShader<DilatedFilterDirectLightingShader>(ini);
            params->View = view->view_common_params_;
            params->UB = UB;
            params->G_Normal = view->G_normal_.Raw();
            params->G_Depth = view->G_depth_.Raw();
            params->HistoryDepthTexture = view->persistent_data_->prev_G_depth.Raw();
            params->HistoryLengthTexture = history_length.Raw();
            params->DilatedFilterInputRadianceTexture = pre_filtered_diffuse_direct_lighting_.Raw();
            TRef<RDGTexture> output_texture = view->denoised_diffuse_direct_lighting_;
            if (i != kMaxPasses - 1) {
                output_texture = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR16G16B16A16_FLOAT);
            }
            params->RWDilatedFilterOutputFilteredRadiance = output_texture.Raw();
            params->PointBorder0Sampler = RHI::Get().GetGlobalSamplers().point_border_0;
            params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
            Helpers::AddComputePass<DilatedFilterDirectLightingShader>(
                builder, shader, params,
                DivideAndRoundUp(view->film_width_, DilatedFilterDirectLightingShader::kTileSize),
                DivideAndRoundUp(view->film_height_, DilatedFilterDirectLightingShader::kTileSize)
            );
        }
    }
}


MI_NAMESPACE_END