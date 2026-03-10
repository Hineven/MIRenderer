/*
* Created: 2025/11/23
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "r_gaussian_radiance_field.h"
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_gaussian_radiance_field.h>
#include <renderer/mi_resource_allocator.h>
#include "r_view_common.h"
#include <cstring>
#include <gtest/internal/gtest-death-test-internal.h>

#include "../include/renderer/r_geometry_buffer.h"
#include "renderer/util/radix_sort.h"
#include "rhi/rhi_buffer.h"
MI_NAMESPACE_BEGIN

CVar<float> CVar_GRF_EmitterIntensityScale(
    "r.grf.emitter_intensity_scale",
    "Scale factor for emitter intensity in Gaussian Radiance Fields.",
    0.5f
);

CVar<bool> CVar_GRF_StochasticRendering(
    "r.grf.stochastic_rendering",
    "Enable stochastic rendering for Gaussian Radiance Fields.",
    true
);

CVar<bool> CVar_GRF_StochasticLargeGaussianHalfResolution(
    "r.grf.stochastic_large_gaussian_half_resolution",
    "Render large stochastic GRF gaussians at half resolution and composite them back.",
    false
);

void GaussianRadianceFieldViewData::Allocate([[maybe_unused]] RenderGraphBuilder &builder, [[maybe_unused]] RendererView *view) {
    // Do nothing for now
}

struct GaussianRadianceFieldUB {
    float GaussianClampingScale;
    float GaussianExpandFactor;
    float StochasticSplitShortAxisThreshold;
    uint32_t Padding;
};

// Shared parameter block
BEGIN_SHADER_PARAMETERS(GaussianRadianceFieldParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(GaussianRadianceFieldUB, UB)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableInverseTransformBuffer)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianSHBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianRadianceFieldHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianRenderableCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianRenderableListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianColorBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianLinearDepthSrcBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianIndirectionSrcBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianNDCPositionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianQuadNDCVector0Buffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianQuadNDCVector1Buffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWSmallGaussianCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWSmallGaussianListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLargeGaussianCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLargeGaussianListBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, DrawGaussianCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianIndirectionBuffer)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Color, {RHIBlendOpType::kBlendAdd, RHIBlendFactorType::kSrcAlpha, RHIBlendFactorType::kOneMinusSrcAlpha})
    SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(GaussianRadianceFieldParameters)

// Shared parameter block
BEGIN_SHADER_PARAMETERS(StochasticDrawGaussianRadianceFieldParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(GaussianRadianceFieldUB, UB)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableInverseTransformBuffer)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianSHBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianRadianceFieldHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianRenderableCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianRenderableListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianColorBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianLinearDepthSrcBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianIndirectionSrcBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianNDCPositionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianQuadNDCVector0Buffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianQuadNDCVector1Buffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWSmallGaussianCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWSmallGaussianListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLargeGaussianCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLargeGaussianListBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, DrawGaussianCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianIndirectionBuffer)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Color, {RHIBlendOpType::kBlendAdd, RHIBlendFactorType::kOne, RHIBlendFactorType::kZero})
    SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth)
END_SHADER_PARAMETERS()


IMPLEMENT_SHADER_PARAMETERS(StochasticDrawGaussianRadianceFieldParameters)

BEGIN_SHADER_PARAMETERS(GRF_CompositeLargeGaussianParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableInverseTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianSHBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianRadianceFieldHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianRenderableListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianListBuffer)
    SHADER_RESOURCE_PARAMETER(Texture2D, LargeGaussianIndex)
    SHADER_RESOURCE_PARAMETER(Texture2D, LargeGaussianDepth)
    SHADER_RESOURCE_PARAMETER(Texture2D, FullResolutionDepth)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWOverlay)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(GRF_CompositeLargeGaussianParameters)

BEGIN_SHADER_PARAMETERS(StochasticDrawLargeGaussianIndexParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(GaussianRadianceFieldUB, UB)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianRenderableListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianLinearDepthSrcBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianNDCPositionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianQuadNDCVector0Buffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveGaussianQuadNDCVector1Buffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, DrawGaussianCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianIndirectionBuffer)
    SHADER_RENDER_TARGET(PixelFormatType::kR32_UINT, ColorIndex, {})
    // SHADER_RENDER_TARGET(PixelFormatType::kR8_UNORM, Alpha, {})
    SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(StochasticDrawLargeGaussianIndexParameters)

class GRF_Shader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
        return {
            "WAVE_SIZE=" + std::to_string(wave_size)
        };
    }
};

class GRF_ClearCountersShader : public GRF_Shader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
    DECLARE_SHADER(GRF_Shader)
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "LARGE_GAUSSIAN_HALF_RESOLUTION_PATH"
        };
    }
};
IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(GRF_ClearCountersShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "ClearCounters");

class GRF_FilterShader : public GRF_Shader {
public:
    DECLARE_SHADER(GRF_Shader)
    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig cfg{};
        cfg.depth_test_enabled = false;
        cfg.depth_write_enabled = false;
        cfg.rasterization_discard = true; // Do not run fragment shaders
        cfg.topology = RHIPrimitiveTopologyType::kPointList;
        return cfg;
    }
};
IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER(GRF_FilterShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "FilterActiveGaussiansVS", "");

class GRF_ProjectShader : public GRF_Shader {
public:
    DECLARE_SHADER(GRF_Shader)
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "LARGE_GAUSSIAN_HALF_RESOLUTION_PATH"
        };
    }
};
IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(GRF_ProjectShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "ProjectActiveGaussians");

class GRF_DrawShader : public GRF_Shader {
public:
    DECLARE_SHADER(GRF_Shader)
    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig cfg{};
        cfg.depth_test_enabled = true;
        cfg.depth_write_enabled = false;
        cfg.depth_compare_op = RHIDepthCompareOpType::kGreater;
        cfg.topology = RHIPrimitiveTopologyType::kPointList;
        return cfg;
    }
};
IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER_GS(GRF_DrawShader,
    "mi/renderer/shaders/GaussianRadianceField.hlsl",
    "DrawActiveGaussians_VS",
    "DrawActiveGaussians_GS",
    "DrawActiveGaussians_PS"
);

class GRF_StochasticDrawShader : public RDGShader {
public:
    static std::vector<std::string> GetShaderDefaultMacros() {
        auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
        return {
            "WAVE_SIZE=" + std::to_string(wave_size)
        };
    }
    RDG_SHADER_USE_PARAMETERS(StochasticDrawGaussianRadianceFieldParameters)
    DECLARE_SHADER()
    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig cfg{};
        cfg.depth_test_enabled = true;
        cfg.depth_write_enabled = true;
        cfg.depth_compare_op = RHIDepthCompareOpType::kGreater;
        cfg.topology = RHIPrimitiveTopologyType::kPointList;
        return cfg;
    }
};
IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER_GS(GRF_StochasticDrawShader,
    "mi/renderer/shaders/GaussianRadianceField.hlsl",
    "StochasticDrawActiveGaussians_VS",
    "StochasticDrawActiveGaussians_GS",
    "StochasticDrawActiveGaussians_PS"
);

class GRF_StochasticDrawLargeGaussianIndexShader : public RDGShader {
public:
    static std::vector<std::string> GetShaderDefaultMacros() {
        auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
        return {
            "WAVE_SIZE=" + std::to_string(wave_size)
        };
    }
    RDG_SHADER_USE_PARAMETERS(StochasticDrawLargeGaussianIndexParameters)
    DECLARE_SHADER()
    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig cfg{};
        cfg.depth_test_enabled = true;
        cfg.depth_write_enabled = true;
        cfg.depth_compare_op = RHIDepthCompareOpType::kGreater;
        cfg.topology = RHIPrimitiveTopologyType::kPointList;
        return cfg;
    }
};
IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER_GS(GRF_StochasticDrawLargeGaussianIndexShader,
    "mi/renderer/shaders/GaussianRadianceField.hlsl",
    "StochasticDrawLargeGaussianIndex_VS",
    "StochasticDrawLargeGaussianIndex_GS",
    "StochasticDrawLargeGaussianIndex_PS"
);

class GRF_CompositeLargeGaussianShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(GRF_CompositeLargeGaussianParameters)
    DECLARE_SHADER()
    constexpr static uint32_t kThreadGroupSize = 8;
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)
        };
    }
};
IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(GRF_CompositeLargeGaussianShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "ComposeLargeGaussians")

static void UpdateScaledViewCommonShaderParameters(ViewCommonShaderParameters & params, uint32_t film_width, uint32_t film_height) {
    auto & camera = params.Camera;
    camera.FilmDimensions = {film_width, film_height};

    float aspect_ratio = float(film_width) / float(film_height);
    camera.FilmAspectRatioAndInvAspectRatio = {aspect_ratio, 1.0f / aspect_ratio};

    uint32_t hzb_size = 1;
    while (hzb_size < film_width || hzb_size < film_height) {
        hzb_size *= 2;
    }
    hzb_size /= 2;
    camera.HZBDimensions = glm::uvec2(hzb_size);
    float film_viewport_world_height = 1.f;
    float film_viewport_world_width = film_viewport_world_height * aspect_ratio;
    camera.FilmPixelWorldSize = {
        1.0f / (film_viewport_world_width * film_width),
        1.0f / (film_viewport_world_height * film_height)
    };

    camera.InvFilmDimensions = {1.0f / float(film_width), 1.0f / float(film_height)};
    camera.UVToHZBScale = {
        (float)film_width / (2 * (float)hzb_size),
        (float)film_height / (2 * (float)hzb_size)
    };
    camera.HZBBaseTexelSize = {
        1.0f / (float)hzb_size,
        1.0f / (float)hzb_size
    };
    camera.HZBToUVScale = {
        2 * (float)hzb_size / (float)film_width,
        2 * (float)hzb_size / (float)film_height
    };
}
void Renderer::Render_PrepareGaussianRadianceFields(RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    // Build per-instance draw indirect commands for FilterActiveGaussians pass
    ctx.gaussian_radiance_fields.draw_indirect_commands.clear();
    // Also build renderable list mapping for instances participating this frame
    auto & renderable_indices = ctx.gaussian_radiance_fields.active_renderable_indices;
    renderable_indices.clear();
    renderable_indices.reserve(ctx.visible_renderables.size());
    // Collect visible gaussian instances
    for (auto & r : ctx.visible_renderables) {
        if (auto inst = r->As<GaussianRadianceFieldInstance>()) {
            auto field = inst->GetField();
            if (!field || field->IsEmpty()) continue;
            RHIDrawIndirectCommand cmd {};
            cmd.vertex_count = field->GetNumPoints();
            cmd.instance_count = 1;
            cmd.first_vertex = 0;
            cmd.first_instance = (uint32_t)renderable_indices.size(); // SV_InstanceID expects this index
            ctx.gaussian_radiance_fields.draw_indirect_commands.push_back(cmd);
            renderable_indices.push_back(inst->GetIndex());
        }
    }
    if (ctx.gaussian_radiance_fields.draw_indirect_commands.empty()) return; // nothing

    // Upload indirect draw commands buffer
    ctx.gaussian_radiance_fields.d_filter_draw_commands = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kIndirect,
        (uint32_t)(ctx.gaussian_radiance_fields.draw_indirect_commands.size() * sizeof(RHIDrawIndirectCommand))
    );
    ctx.gaussian_radiance_fields.d_filter_draw_commands->SetName("GaussianFilterDrawCommands");
    view->upload_context_.Add(ctx.gaussian_radiance_fields.d_filter_draw_commands.Raw(),
        ctx.gaussian_radiance_fields.draw_indirect_commands.data(),
        ctx.gaussian_radiance_fields.draw_indirect_commands.size() * sizeof(RHIDrawIndirectCommand));
    view->upload_context_.AddExtraBarrier(ctx.gaussian_radiance_fields.d_filter_draw_commands.Raw());

    // Create and upload active renderable list/count buffers for this frame
    ctx.gaussian_radiance_fields.d_active_renderable_list_buffer = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        (uint32_t)(renderable_indices.size() * sizeof(uint32_t))
    );
    ctx.gaussian_radiance_fields.d_active_renderable_list_buffer->SetName("ActiveGaussianRenderableListBuffer");
    ctx.gaussian_radiance_fields.d_active_renderable_count_buffer = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        sizeof(uint32_t)
    );
    ctx.gaussian_radiance_fields.d_active_renderable_count_buffer->SetName("ActiveGaussianRenderableCountBuffer");
    auto & renderable_count = ctx.gaussian_radiance_fields.active_renderable_count;
    renderable_count = (uint32_t)renderable_indices.size();
    view->upload_context_.Add(ctx.gaussian_radiance_fields.d_active_renderable_list_buffer.Raw(), renderable_indices.data(), renderable_indices.size()*sizeof(uint32_t));
    view->upload_context_.AddExtraBarrier(ctx.gaussian_radiance_fields.d_active_renderable_list_buffer.Raw());
    view->upload_context_.Add(ctx.gaussian_radiance_fields.d_active_renderable_count_buffer.Raw(), &renderable_count, sizeof(uint32_t));
    view->upload_context_.AddExtraBarrier(ctx.gaussian_radiance_fields.d_active_renderable_count_buffer.Raw());
}

void Renderer::Render_DrawGaussianRadianceFields(
    RendererView * view, RenderGraphBuilder & builder
) {
    RDGSectionGuard section(builder, "Render_DrawGaussianRadianceFields");

    // Collect visible instances and derive total gaussian count & per-instance offsets from device headers
    // TODO maintain a persistent list of visible instances to avoid this every frame.
    // (host allocator context)
    struct InstanceInfo { GaussianRadianceFieldInstance * inst; uint32_t field_index; uint32_t base_offset; uint32_t count; };
    std::vector<InstanceInfo> instances;
    uint32_t total_gaussians = 0;
    for (auto & r : ctx.visible_renderables) {
        if (auto inst = r->As<GaussianRadianceFieldInstance>()) {
            auto field = inst->GetField();
            if (!field || field->IsEmpty()) continue;
            auto dev = field->GetDeviceField();
            instances.push_back({inst, dev->GetIndex(), dev->GetPointOffset(), field->GetNumPoints()});
            total_gaussians += field->GetNumPoints();
        }
    }
    if (instances.empty()) return; // Nothing to render

    // Import persistent packed gaussian & SH buffers from allocator uber buffers
    auto packed_gaussians_buffer = builder.Import(
        device_allocator_->GetCustomUberBuffer(GaussianRadianceField::kGaussianRadianceAllocatorUberBufferIndex)->GetRHI()
    );
    auto sh_buffer = builder.Import(
        device_allocator_->GetCustomUberBuffer(GaussianRadianceField::kGaussianRadianceSHAllocatorUberBufferIndex)->GetRHI()
    );

    // Active buffers sized by total_gaussians
    auto active_gaussian_color_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
    auto active_gaussian_count_buffer = builder.CreateBuffer<uint32_t>();
    auto active_gaussian_list_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
    auto active_gaussian_linear_depth_src_buffer = builder.CreateBuffer<float>(total_gaussians);
    auto active_gaussian_linear_depth_dst_buffer = builder.CreateBuffer<float>(total_gaussians);
    auto active_gaussian_indirection_src_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
    auto active_gaussian_indirection_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
    auto active_gaussian_ndc_pos_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
    auto active_gaussian_quad_vec0_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
    auto active_gaussian_quad_vec1_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
    bool enable_multires_stochastic = CVar_GRF_StochasticRendering.Get() && CVar_GRF_StochasticLargeGaussianHalfResolution.Get();
    TRef<RDGBuffer> small_gaussian_count_buffer;
    TRef<RDGBuffer> small_gaussian_list_buffer;
    TRef<RDGBuffer> large_gaussian_count_buffer;
    TRef<RDGBuffer> large_gaussian_list_buffer;
    if (enable_multires_stochastic) {
        small_gaussian_count_buffer = builder.CreateBuffer<uint32_t>();
        small_gaussian_list_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
        large_gaussian_count_buffer = builder.CreateBuffer<uint32_t>();
        large_gaussian_list_buffer = builder.CreateBuffer<uint32_t>(total_gaussians);
    }

    TRef<RDGTexture> testing_depth = view->g_buffer_->G_depth_;
    if (CVar_GRF_StochasticRendering.Get()) {
        // Duplicate depth buffer for stochastic rendering, because we're writing to it as well
        testing_depth = builder.CreateTexture2D(
            view->film_width_,
            view->film_height_,
            PixelFormatType::kD32_FLOAT,
            RHITextureUsageFlagBits::kDepthStencil | RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kTransfer
        );
        // Save to view for external usage
        view->grf_->stochastic_rendering_depth_ = testing_depth;
        view->grf_->stochastic_rendering_opacity_ = builder.CreateTexture2D(
            view->film_width_,
            view->film_height_,
            PixelFormatType::kR8_UNORM,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kTransfer
        );
        // Copy
        Helpers::CopyTexture(builder, view->g_buffer_->G_depth_.Raw(), testing_depth.Raw());
        // Clear
        Helpers::Clear(builder, view->grf_->stochastic_rendering_opacity_.Raw());
    }

    auto params = builder.Allocate<GaussianRadianceFieldParameters>();
    auto UB = builder.Allocate<GaussianRadianceFieldUB>();
    {
        UB->GaussianClampingScale = 1e-3f;
        UB->GaussianExpandFactor  = 3.5f;
        UB->StochasticSplitShortAxisThreshold = 10.0f;
        UB->Padding = 0;
    }
    params->UB=UB;
    params->View=view->view_common_params_;
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    params->RenderableInverseTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_inverse_transforms_.Raw());
    // Bind resources
    params->Gaussian3DBuffer = packed_gaussians_buffer;
    params->GaussianSHBuffer = sh_buffer;
    params->GaussianRadianceFieldHeaderBuffer = builder.Import(
        device_allocator_->GetGaussianRadianceFieldHeaderBuffer()
    );
    params->ActiveGaussianRenderableCount = ctx.gaussian_radiance_fields.d_active_renderable_count_buffer.Raw();
    params->ActiveGaussianRenderableListBuffer = ctx.gaussian_radiance_fields.d_active_renderable_list_buffer.Raw();
    params->RWActiveGaussianColorBuffer = active_gaussian_color_buffer.Raw();
    params->RWActiveGaussianCount = active_gaussian_count_buffer.Raw();
    params->RWActiveGaussianListBuffer = active_gaussian_list_buffer.Raw();
    params->RWActiveGaussianLinearDepthSrcBuffer = active_gaussian_linear_depth_src_buffer.Raw();
    params->RWActiveGaussianIndirectionSrcBuffer = active_gaussian_indirection_src_buffer.Raw();
    params->RWActiveGaussianNDCPositionBuffer = active_gaussian_ndc_pos_buffer.Raw();
    params->RWActiveGaussianQuadNDCVector0Buffer = active_gaussian_quad_vec0_buffer.Raw();
    params->RWActiveGaussianQuadNDCVector1Buffer = active_gaussian_quad_vec1_buffer.Raw();
    params->RWSmallGaussianCount = small_gaussian_count_buffer.Raw();
    params->RWSmallGaussianListBuffer = small_gaussian_list_buffer.Raw();
    params->RWLargeGaussianCount = large_gaussian_count_buffer.Raw();
    params->RWLargeGaussianListBuffer = large_gaussian_list_buffer.Raw();
    params->DrawGaussianCount = active_gaussian_count_buffer.Raw();
    if (CVar_GRF_StochasticRendering.Get()) {
        // For stochastic rendering, we don't sort, so indirection src buffer is not needed
        params->ActiveGaussianIndirectionBuffer = active_gaussian_indirection_src_buffer.Raw();
    } else {
        params->ActiveGaussianIndirectionBuffer = active_gaussian_indirection_buffer.Raw();
    }
    params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    // Draw to linear color overlay
    params->Color = view->overlay_.Raw();
    // Test against the depth buffer.
    params->Depth = testing_depth.Raw();

    auto & lib = RDGShaderLibrary::Get();
    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
    RDGShaderInitializationInfo stochastic_large_gaussian_ini {};
    if (enable_multires_stochastic) {
        stochastic_large_gaussian_ini.optional_macros.push_back("LARGE_GAUSSIAN_HALF_RESOLUTION_PATH");
    }
    // Pass 1: ClearCounters
    auto clear_shader = lib.GetShader<GRF_ClearCountersShader>(stochastic_large_gaussian_ini);
    Helpers::AddComputePass(builder, clear_shader, params);
    // Pass 2: FilterActiveGaussians
    if (ctx.gaussian_radiance_fields.d_filter_draw_commands) {
        auto shader = lib.GetShader<GRF_FilterShader>();
        Helpers::AddDrawIndirectPass(builder, shader, params,
            ctx.gaussian_radiance_fields.d_filter_draw_commands.Raw(),
            (uint32_t)ctx.gaussian_radiance_fields.draw_indirect_commands.size());
    }
    // Pass 3: ProjectActiveGaussians
    {
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, active_gaussian_count_buffer.Raw(), wave_size);
        auto shader = lib.GetShader<GRF_ProjectShader>(stochastic_large_gaussian_ini);
        Helpers::AddComputeIndirectPass(builder, shader, params, cmd.Raw());
    }
    if (!CVar_GRF_StochasticRendering.Get()) {
        // Pass 4: Radix Sort active gaussians by linear depth
        DeviceRadixSort::AddRadixSort32BitsPass(builder, total_gaussians,
            active_gaussian_linear_depth_src_buffer.Raw(), active_gaussian_linear_depth_dst_buffer.Raw(),
            active_gaussian_indirection_src_buffer.Raw(), active_gaussian_indirection_buffer.Raw(),
            active_gaussian_count_buffer.Raw()
        );
        // Pass 5: DrawActiveGaussians
        {
            auto final_draw_command = Helpers::SpawnDrawIndirectCommand(builder, active_gaussian_count_buffer.Raw());
            auto shader = lib.GetShader<GRF_DrawShader>();
            params->DrawGaussianCount = active_gaussian_count_buffer.Raw();
            params->ActiveGaussianIndirectionBuffer = active_gaussian_indirection_buffer.Raw();
            Helpers::AddDrawIndirectPass(builder, shader, params, final_draw_command.Raw());
        }
    } else {
        if (!enable_multires_stochastic) {
            auto final_draw_command = Helpers::SpawnDrawIndirectCommand(builder, active_gaussian_count_buffer.Raw());
            auto shader = lib.GetShader<GRF_StochasticDrawShader>();
            auto stochastic_params = builder.Allocate<StochasticDrawGaussianRadianceFieldParameters>();
            *stochastic_params = *(StochasticDrawGaussianRadianceFieldParameters*)params;
            stochastic_params->DrawGaussianCount = active_gaussian_count_buffer.Raw();
            stochastic_params->ActiveGaussianIndirectionBuffer = active_gaussian_indirection_src_buffer.Raw();
            Helpers::AddDrawIndirectPass(builder, shader, stochastic_params, final_draw_command.Raw());
        } else {
            auto shader = lib.GetShader<GRF_StochasticDrawShader>();

            auto small_draw_command = Helpers::SpawnDrawIndirectCommand(builder, small_gaussian_count_buffer.Raw());
            auto small_params = builder.Allocate<StochasticDrawGaussianRadianceFieldParameters>();
            *small_params = *(StochasticDrawGaussianRadianceFieldParameters*)params;
            small_params->DrawGaussianCount = small_gaussian_count_buffer.Raw();
            small_params->ActiveGaussianIndirectionBuffer = small_gaussian_list_buffer.Raw();
            Helpers::AddDrawIndirectPass(builder, shader, small_params, small_draw_command.Raw());

            uint32_t half_width = DivideAndRoundUp(view->film_width_, 2u);
            uint32_t half_height = DivideAndRoundUp(view->film_height_, 2u);
            auto large_index = builder.CreateTexture2D(
                half_width,
                half_height,
                PixelFormatType::kR32_UINT,
                RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kRenderTarget
            );
            auto large_depth = builder.CreateTexture2D(
                half_width,
                half_height,
                PixelFormatType::kD32_FLOAT,
                RHITextureUsageFlagBits::kDepthStencil | RHITextureUsageFlagBits::kShaderResource
            );
            auto large_alpha = builder.CreateTexture2D(
                half_width,
                half_height,
                PixelFormatType::kR8_UNORM,
                RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kRenderTarget
            );
            // Prepare for export
            view->grf_->stochastic_rendering_opacity_large_ = large_alpha;

            auto half_res_view = builder.Allocate<ViewCommonShaderParameters>();
            *half_res_view = *view->view_common_params_;
            UpdateScaledViewCommonShaderParameters(*half_res_view, half_width, half_height);

            auto large_draw_command = Helpers::SpawnDrawIndirectCommand(builder, large_gaussian_count_buffer.Raw());
            auto large_shader = lib.GetShader<GRF_StochasticDrawLargeGaussianIndexShader>();
            auto large_params = builder.Allocate<StochasticDrawLargeGaussianIndexParameters>();
            large_params->View = half_res_view;
            large_params->UB = UB;
            large_params->RenderableTransformBuffer = params->RenderableTransformBuffer;
            large_params->Gaussian3DBuffer = params->Gaussian3DBuffer;
            large_params->ActiveGaussianRenderableListBuffer = params->ActiveGaussianRenderableListBuffer;
            large_params->RWActiveGaussianListBuffer = params->RWActiveGaussianListBuffer;
            large_params->RWActiveGaussianLinearDepthSrcBuffer = params->RWActiveGaussianLinearDepthSrcBuffer;
            large_params->RWActiveGaussianNDCPositionBuffer = params->RWActiveGaussianNDCPositionBuffer;
            large_params->RWActiveGaussianQuadNDCVector0Buffer = params->RWActiveGaussianQuadNDCVector0Buffer;
            large_params->RWActiveGaussianQuadNDCVector1Buffer = params->RWActiveGaussianQuadNDCVector1Buffer;
            large_params->DrawGaussianCount = large_gaussian_count_buffer.Raw();
            large_params->ActiveGaussianIndirectionBuffer = large_gaussian_list_buffer.Raw();
            large_params->ColorIndex = large_index.Raw();
            large_params->ColorIndex.load_op = RHILoadOpType::kClear;
            large_params->ColorIndex.clear_value = {0, 0, 0, 0};
            large_params->Depth = large_depth.Raw();
            large_params->Depth.load_op = RHILoadOpType::kClear;
            large_params->Depth.clear_value = {0, 0, 0, 0};
            // large_params->Alpha = large_alpha.Raw();
            // large_params->Alpha.load_op = RHILoadOpType::kClear;
            // large_params->Alpha.clear_value = {0, 0, 0, 0};

            Helpers::AddDrawIndirectPass(builder, large_shader, large_params, large_draw_command.Raw());

            auto composite_shader = lib.GetShader<GRF_CompositeLargeGaussianShader>();
            auto composite_params = builder.Allocate<GRF_CompositeLargeGaussianParameters>();
            composite_params->View = view->view_common_params_;
            composite_params->RenderableHeaderBuffer = params->RenderableHeaderBuffer;
            composite_params->RenderableInverseTransformBuffer = params->RenderableInverseTransformBuffer;
            composite_params->Gaussian3DBuffer = params->Gaussian3DBuffer;
            composite_params->GaussianSHBuffer = params->GaussianSHBuffer;
            composite_params->GaussianRadianceFieldHeaderBuffer = params->GaussianRadianceFieldHeaderBuffer;
            composite_params->ActiveGaussianRenderableListBuffer = params->ActiveGaussianRenderableListBuffer;
            composite_params->RWActiveGaussianListBuffer = params->RWActiveGaussianListBuffer;
            composite_params->LargeGaussianIndex = large_index.Raw();
            composite_params->LargeGaussianDepth = large_depth.Raw();
            composite_params->FullResolutionDepth = testing_depth.Raw();
            composite_params->RWOverlay = view->overlay_.Raw();
            Helpers::AddComputePass(
                builder,
                composite_shader,
                composite_params,
                DivideAndRoundUp(view->film_width_, GRF_CompositeLargeGaussianShader::kThreadGroupSize),
                DivideAndRoundUp(view->film_height_, GRF_CompositeLargeGaussianShader::kThreadGroupSize)
            );
        }
    }

}

// Delete legacy definitions above this line in final cleanup.

MI_NAMESPACE_END
