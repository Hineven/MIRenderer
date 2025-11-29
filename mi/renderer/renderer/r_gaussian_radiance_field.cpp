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

#include "renderer/util/radix_sort.h"
#include "rhi/rhi_buffer.h"
MI_NAMESPACE_BEGIN

CVar<float> CVar_GRF_EmitterIntensityScale(
    "r.grf.emitter_intensity_scale",
    "Scale factor for emitter intensity in Gaussian Radiance Fields.",
    0.5f
);

struct GaussianRadianceFieldUB {
    float GaussianClampingScale;
    float GaussianExpandFactor;
    glm::uvec2 Padding;
    glm::mat4x4 LightWorldToNDC;
};

// Shared parameter block
BEGIN_SHADER_PARAMETERS(GaussianRadianceFieldParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(GaussianRadianceFieldUB, UB)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)

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
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveGaussianIndirectionBuffer)
    SHADER_RESOURCE_PARAMETER(Texture2D, ShadowMapTexture)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Color, {RHIBlendOpType::kBlendAdd, RHIBlendFactorType::kSrcAlpha, RHIBlendFactorType::kOneMinusSrcAlpha})
    SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth)
END_SHADER_PARAMETERS()

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
};
IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(GRF_ClearCountersShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "ClearCounters");

class GRF_FilterShader : public GRF_Shader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
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
IMPLEMENT_RDG_GRAPHICS_SHADER(GRF_FilterShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "FilterActiveGaussiansVS", "");

class GRF_ProjectShader : public GRF_Shader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
    DECLARE_SHADER(GRF_Shader)
};
IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(GRF_ProjectShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "ProjectActiveGaussians");

class GRF_DrawShader : public GRF_Shader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
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

void Renderer::Render_PrepareGaussianRadianceFields(RendererView *view, RenderGraphBuilder &builder) {
    // Build per-instance draw indirect commands for FilterActiveGaussians pass
    ctx.gaussian_radiance_fields.draw_indirect_commands.clear();
    auto & allocator = *device_allocator_;
    // Also build renderable list mapping for instances participating this frame
    auto & renderable_indices = ctx.gaussian_radiance_fields.active_renderable_indices;
    renderable_indices.clear();
    renderable_indices.reserve(ctx.visible_renderables.size());
    // Collect visible gaussian instances
    for (auto & r : ctx.visible_renderables) {
        if (auto inst = r->As<GaussianRadianceFieldInstance>()) {
            auto field = inst->GetField();
            if (!field || field->IsEmpty()) continue;
            auto dev = field->GetDeviceField();
            uint32_t slot = dev->GetIndex();
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

    auto params = builder.Allocate<GaussianRadianceFieldParameters>();
    auto UB = builder.Allocate<GaussianRadianceFieldUB>();
    {
        UB->GaussianClampingScale = 1e-3f;
        UB->GaussianExpandFactor  = 2.25f;
        UB->Padding[0] = UB->Padding[1] = 0;
        UB->LightWorldToNDC = view->shadow_mapping_.light_world_to_ndc_;
    }
    params->UB = UB;
    params->View=view->view_common_params_;
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
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
    params->ActiveGaussianIndirectionBuffer = active_gaussian_indirection_buffer.Raw();
    params->ShadowMapTexture = view->shadow_map_moments_.Raw();
    params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    // Draw to linear color overlay
    params->Color = view->overlay_.Raw();
    // Test against the depth buffer.
    params->Depth = view->G_depth_.Raw();

    auto & lib = RDGShaderLibrary::Get();
    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
    // Pass 1: ClearCounters
    auto clear_shader = lib.GetShader<GRF_ClearCountersShader>();
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
        auto shader = lib.GetShader<GRF_ProjectShader>();
        Helpers::AddComputeIndirectPass(builder, shader, params, cmd.Raw());
    }
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
        Helpers::AddDrawIndirectPass(builder, shader, params, final_draw_command.Raw());
    }

}

// Delete legacy definitions above this line in final cleanup.

MI_NAMESPACE_END
