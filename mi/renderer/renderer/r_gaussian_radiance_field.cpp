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
MI_NAMESPACE_BEGIN
struct GaussianRadianceFieldUB {
    float GaussianClampingScale;
    float GaussianExpandFactor;
    glm::uvec2 Padding;
};

// Shared parameter block
BEGIN_SHADER_PARAMETERS(GaussianRadianceFieldParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(GaussianRadianceFieldUB, UB)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianSHBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, InstanceGaussianIndexOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, InstanceGaussianCountBuffer)
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
    SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Color, {})
END_SHADER_PARAMETERS()

class GRF_ClearCountersShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
    DECLARE_SHADER()
    constexpr static uint32_t kThreadGroupSize = 128;
    static std::vector<std::string> GetShaderDefaultMacros() { return {"THREAD_GROUP_SIZE=128"}; }
};
IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(GRF_ClearCountersShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "ClearCounters");

class GRF_FilterShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
    DECLARE_SHADER()
    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig cfg{};
        cfg.depth_test_enabled = false;
        cfg.depth_write_enabled = false;
        cfg.rasterization_discard = true; // Do not run fragment shaders
        cfg.topology = RHIPrimitiveTopologyType::kPointList;
        return cfg;
    }
};
IMPLEMENT_RDG_GRAPHICS_SHADER(GRF_FilterShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "FilterActiveGaussians", "");

class GRF_ProjectShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
    DECLARE_SHADER()
};
IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(GRF_ProjectShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "ProjectActiveGaussians");

class GRF_DrawShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(GaussianRadianceFieldParameters)
    DECLARE_SHADER()
    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig cfg{};
        cfg.depth_test_enabled = true;
        cfg.depth_write_enabled = false;
        cfg.depth_compare_op = RHIDepthCompareOpType::kGreater;
        cfg.topology = RHIPrimitiveTopologyType::kPointList;
        return cfg;
    }
};
IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER(GRF_DrawShader, "mi/renderer/shaders/GaussianRadianceField.hlsl", "DrawActiveGaussians", "DrawActiveGaussians");

void Renderer::Render_PrepareGaussianRadianceFields(RendererView *view, RenderGraphBuilder &builder) {
    // Build per-instance draw indirect commands for FilterActiveGaussians pass
    ctx.gaussian_radiance_fields.draw_indirect_commands.clear();
    auto & allocator = *device_allocator_;
    // Prepare host side arrays for persistent instance offset/count buffers
    std::vector<uint32_t> instance_offsets(DeviceBindlessResourceAllocator::kMaxNumGaussianRadianceFields, 0);
    std::vector<uint32_t> instance_counts(DeviceBindlessResourceAllocator::kMaxNumGaussianRadianceFields, 0);
    // Also build renderable list mapping for instances participating this frame
    std::vector<uint32_t> renderable_indices;
    renderable_indices.reserve(ctx.visible_renderables.size());
    // Collect visible gaussian instances
    for (auto & r : ctx.visible_renderables) {
        if (auto inst = r->As<GaussianRadianceFieldInstance>()) {
            auto field = inst->GetField();
            if (!field || field->IsEmpty()) continue;
            auto dev = field->GetDeviceField();
            uint32_t slot = dev->GetIndex();
            instance_offsets[slot] = dev->GetPointOffset();
            instance_counts[slot] = field->GetNumPoints();
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

    // Upload persistent instance buffers via BatchedUploadContext
    auto offsets_rdg = builder.Import(allocator.GetGaussianInstanceOffsetBuffer(), RHIGPUAccessFlagBits::kAll, RHIPipelineStageFlagBits::kAll);
    auto counts_rdg  = builder.Import(allocator.GetGaussianInstanceCountBuffer(), RHIGPUAccessFlagBits::kAll, RHIPipelineStageFlagBits::kAll);
    view->upload_context_.Add(offsets_rdg, instance_offsets.data(), instance_offsets.size()*sizeof(uint32_t));
    view->upload_context_.Add(counts_rdg, instance_counts.data(), instance_counts.size()*sizeof(uint32_t));

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
    uint32_t renderable_count = (uint32_t)renderable_indices.size();
    view->upload_context_.Add(ctx.gaussian_radiance_fields.d_active_renderable_list_buffer.Raw(), renderable_indices.data(), renderable_indices.size()*sizeof(uint32_t));
    view->upload_context_.Add(ctx.gaussian_radiance_fields.d_active_renderable_count_buffer.Raw(), &renderable_count, sizeof(uint32_t));
    view->upload_context_.AddExtraBarrier(ctx.gaussian_radiance_fields.d_active_renderable_list_buffer.Raw());
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

    // Remove previous temporary per-frame buffers and use persistent ones from allocator
    auto instance_offset_buffer = builder.Import(device_allocator_->GetGaussianInstanceOffsetBuffer());
    auto instance_count_buffer = builder.Import(device_allocator_->GetGaussianInstanceCountBuffer());
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
        UB->GaussianClampingScale = 0.3f;
        UB->GaussianExpandFactor  = 2.25f;
        UB->Padding[0] = UB->Padding[1] = 0;
    }
    params->UB=UB;
    params->View=view->view_common_params_;
    // Bind resources
    params->Gaussian3DBuffer = packed_gaussians_buffer;
    params->GaussianSHBuffer = sh_buffer;
    params->InstanceGaussianIndexOffsetBuffer = instance_offset_buffer;
    params->InstanceGaussianCountBuffer = instance_count_buffer;
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
    params->G_Depth = view->G_depth_.Raw();
    params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    // Draw to back buffer directly.
    params->Color = builder.Import(RHI::Get().GetBackBuffer());

    auto & lib = RDGShaderLibrary::Get();
    // Pass 1: ClearCounters
    auto clear_shader = lib.GetShader<GRF_ClearCountersShader>();
    Helpers::AddComputePass(builder, clear_shader, params, 1);
    // Pass 2: FilterActiveGaussians
    if (ctx.gaussian_radiance_fields.d_filter_draw_commands) {
        auto filter_shader = lib.GetShader<GRF_FilterShader>();
        builder.AddPass<GRF_FilterShader>({}, filter_shader, params,
            [filter_shader, params, draw_buffer=ctx.gaussian_radiance_fields.d_filter_draw_commands.Raw(), draw_count=(uint32_t)ctx.gaussian_radiance_fields.draw_indirect_commands.size()](RDGPass* pass, RHICommandQueueGraphics &q){
                if (auto ctx = RDGCommandHelper::BindGraphicsShader<GRF_FilterShader>(q, pass, filter_shader, params)) {
                    q.BeginRendering();
                    // No vertex buffer binding needed; vertex shader fetches from buffers directly.
                    q.DrawIndirect(draw_buffer->GetRHI(), draw_count);
                    q.EndRendering();
                }
            }
        );
    }
    // Pass 3: Radix Sort active gaussians by linear depth
    DeviceRadixSort::AddRadixSort32BitsPass(builder, total_gaussians,
        active_gaussian_linear_depth_src_buffer.Raw(), active_gaussian_linear_depth_dst_buffer.Raw(),
        active_gaussian_indirection_src_buffer.Raw(), active_gaussian_indirection_buffer.Raw(),
        active_gaussian_count_buffer.Raw()
    );
    // Pass 4: ProjectActiveGaussians
    auto project_shader = lib.GetShader<GRF_ProjectShader>();
    auto groups = DivideAndRoundUp(total_gaussians, GRF_ClearCountersShader::kThreadGroupSize);
    Helpers::AddComputePass(builder, project_shader, params, groups);
    // Pass 5: DrawActiveGaussians
    auto final_draw_command = Helpers::SpawnDrawIndirectCommand(builder, active_gaussian_count_buffer.Raw());
    auto draw_shader = lib.GetShader<GRF_DrawShader>();
    builder.AddPass<GRF_DrawShader>({}, draw_shader, params,
        [draw_shader, params, draw_buffer=final_draw_command.Raw(), draw_count=(uint32_t)ctx.gaussian_radiance_fields.draw_indirect_commands.size()](RDGPass* pass, RHICommandQueueGraphics &q){
            if (auto ctx = RDGCommandHelper::BindGraphicsShader<GRF_DrawShader>(q, pass, draw_shader, params, true)) {
                q.BeginRendering();
                q.DrawIndirect(draw_buffer->GetRHI());
                q.EndRendering();
            }
        }
    );
}

// Delete legacy definitions above this line in final cleanup.

MI_NAMESPACE_END
