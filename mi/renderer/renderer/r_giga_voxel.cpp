/*
 * Created: 2026/06/19
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>
#include <algorithm>

#include <renderer/mi_renderer.h>
#include <renderer/mi_giga_voxel.h>
#include <rhi/rhi_buffer.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/r_geometry_buffer.h>

#include "r_view_common.h"

MI_NAMESPACE_BEGIN

// =============================================================================
// DrawGigaVoxelVCShader — HW raster pass for GigaVoxel VC chunk geometry.
//
// Renders greedy-meshed VC triangles into the shared G_visibility_ + G_depth_
// (both kLoad, layered on top of the static-mesh deferred pass). PS encodes
// the unified GigaVoxel VC payload for DecodeVisibility.
// =============================================================================
class DrawGigaVoxelVCShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        // Per-draw (RenderableIndex, GlobalChunkIndex).
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableIndexAndChunkIndexBuffer)

        // GigaVoxelVertex position attribute only (VS reads position directly;
        // normal/uv_base/uv_scale are resolved at decode time from the global
        // vertex/index buffers).
        SHADER_VERTEX_BUFFER(sizeof(GigaVoxelVertex), VertexBuffer)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(GigaVoxelVertex, position),
                                RHIVertexAttributeFormatType::k3xFp32, position)

        SHADER_RENDER_TARGET(PixelFormatType::kR32G32B32A32_UINT, Visibility, {})
        SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth, {})
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig config {};
        // Reversed-z depth buffer (shared with the static-mesh deferred pass).
        config.depth_compare_op = RHIDepthCompareOpType::kGreater;
        return config;
    }
};

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawGigaVoxelVCShader,
    "mi/renderer/shaders/DrawGigaVoxel.hlsl", "DrawGigaVoxelVCVS", "DrawGigaVoxelVCPS");

// =============================================================================
// Render_PrepareGigaVoxel — build per-chunk indirect draw commands.
//
// One draw per non-empty chunk across all visible GigaVoxelInstance renderables.
// Each draw references the global vertex/index uber buffer with the chunk's own
// {vertex_offset, index_offset} span (chunk-local indices, 0-based). A companion
// buffer stores (RenderableIndex, GlobalChunkIndex) per draw for the VS.
// =============================================================================
void Renderer::Render_PrepareGigaVoxel(RendererView * view, [[maybe_unused]] RenderGraphBuilder & builder) {
    auto & data = ctx.giga_voxel_vc;
    data.draw_invocation_sorting_headers.clear();
    data.draw_indirect_commands.clear();

    auto SpawnDraws = [&] {
        for (auto & e : ctx.visible_renderables) {
            auto gv_inst = e->As<GigaVoxelInstance>();
            if (!gv_inst) continue;
            auto * gv = gv_inst->GetGigaVoxel();
            if (!gv) continue;
            uint32_t renderable_index = e->GetIndex();
            for (const auto & [id, h] : gv->GetChunkHandles()) {
                (void)id;
                if (!h.valid || h.IsEmpty()) continue;
                RHIDrawIndexedIndirectCommand cmd {};
                cmd.first_instance = 0; // filled after sorting
                // Index/vertex offsets are the chunk's span in the GLOBAL uber
                // buffers (element units). first_index is relative to the bound
                // index buffer start (== the global index uber buffer base).
                cmd.first_index = h.index_offset;
                cmd.vertex_offset = static_cast<int32_t>(h.vertex_offset);
                cmd.instance_count = 1;
                cmd.index_count = h.index_count;

                GigaVoxelDrawHeader hdr {};
                hdr.renderable_index = renderable_index;
                hdr.global_chunk_index = h.chunk_header_index;
                hdr.indirect_command = cmd;
                data.draw_invocation_sorting_headers.push_back(hdr);
            }
        }
    };
    SpawnDraws();

    if (data.draw_invocation_sorting_headers.empty()) {
        // Nothing to draw. Still register empty buffers to keep downstream
        // binding stable (avoid read-before-write on the indirect buffer).
        data.d_draw_commands = RDGBuffer::Create(
            RHIBufferUsageFlagBits::kIndirect, sizeof(RHIDrawIndexedIndirectCommand));
        data.d_draw_commands->SetName("GigaVoxelVCDrawCommandsBuffer");
        data.d_renderable_chunk_indices = RDGBuffer::Create(
            RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * 2);
        data.d_renderable_chunk_indices->SetName("GigaVoxelVCRenderableChunkIndexBuffer");
        view->upload_context_.AddExtraBarrier(data.d_draw_commands.Raw());
        view->upload_context_.AddExtraBarrier(data.d_renderable_chunk_indices.Raw());
        return;
    }

    data.d_draw_commands = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kIndirect,
        sizeof(RHIDrawIndexedIndirectCommand) * data.draw_invocation_sorting_headers.size());
    data.d_draw_commands->SetName("GigaVoxelVCDrawCommandsBuffer");
    data.d_renderable_chunk_indices = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        sizeof(uint32_t) * 2 * data.draw_invocation_sorting_headers.size());
    data.d_renderable_chunk_indices->SetName("GigaVoxelVCRenderableChunkIndexBuffer");

    {
        data.draw_indirect_commands.reserve(data.draw_invocation_sorting_headers.size());
        auto * renderable_chunk = (uint32_t *)view->temp_allocator_.Allocate(
            data.draw_invocation_sorting_headers.size() * sizeof(uint32_t) * 2);
        for (auto [i, h] : std::views::enumerate(data.draw_invocation_sorting_headers)) {
            h.indirect_command.first_instance = (uint32_t)i;
            data.draw_indirect_commands.push_back(h.indirect_command);
            renderable_chunk[i * 2] = h.renderable_index;
            renderable_chunk[i * 2 + 1] = h.global_chunk_index;
        }
        auto size = sizeof(RHIDrawIndexedIndirectCommand) * data.draw_indirect_commands.size();
        view->upload_context_.Add(data.d_draw_commands.Raw(), data.draw_indirect_commands.data(), size);
        view->upload_context_.Add(data.d_renderable_chunk_indices.Raw(),
            renderable_chunk, data.draw_indirect_commands.size() * sizeof(uint32_t) * 2);
        view->upload_context_.AddExtraBarrier(data.d_draw_commands.Raw());
        view->upload_context_.AddExtraBarrier(data.d_renderable_chunk_indices.Raw());
    }
}

// =============================================================================
// Render_DrawGigaVoxelVC — rasterize VC chunks into the shared visibility/depth.
//
// Runs AFTER Render_DrawDeferredStaticMeshes (which clears visibility+depth).
// Both targets are kLoad here; HW depth test (reversed-z) resolves occlusion
// against static meshes. A single global vertex/index uber buffer is bound, and
// one DrawIndexedIndirect covers all chunk draws (offsets per-command).
// =============================================================================
void Renderer::Render_DrawGigaVoxelVC(RendererView * view, RenderGraphBuilder & builder) {
    auto & data = ctx.giga_voxel_vc;
    if (data.draw_indirect_commands.empty()) return;

    RDGSectionGuard section_guard(builder, "Render_DrawGigaVoxelVC");
    auto params = builder.Allocate<DrawGigaVoxelVCShader::Params>();
    params->View = view->view_common_params_;
    params->RenderableIndexAndChunkIndexBuffer = data.d_renderable_chunk_indices.Raw();

    // Shared visibility + depth targets (kLoad, layered on the static-mesh pass).
    params->Visibility = view->g_buffer_->G_visibility_.Raw();
    params->Visibility.load_op = RHILoadOpType::kLoad;
    params->Depth = view->g_buffer_->G_depth_.Raw();
    params->Depth.load_op = RHILoadOpType::kLoad;

    auto shader = RDGShaderLibrary::Get().GetShader<DrawGigaVoxelVCShader>();
    auto * heap = GigaVoxel::GetGlobalGeometryHeap();
    auto * gpu_vbuf = heap->GetGPUVertexBuffer();
    auto * gpu_ibuf = heap->GetGPUIndexBuffer();

    auto raster_pass = builder.AddPass<DrawGigaVoxelVCShader>({}, shader, params,
        [params, shader, data, gpu_vbuf, gpu_ibuf, rdg_draw_cmd = data.d_draw_commands.Raw()]
        ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            auto tid = pass->GetParameterTableId();
            RDGCommandHelper::BeginGraphicsRender(queue, shader, tid,
                &DrawGigaVoxelVCShader::GetShaderParamStructInfo()->render_pass_info_, params);
            // Single global vertex/index uber buffer; per-chunk spans are in the
            // indirect commands (first_index / vertex_offset).
            queue.SetCullMode(RHICullModeType::kBack);
            queue.BindVertexBuffer(0, gpu_vbuf->GetSpan());
            RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
            queue.DrawIndexedIndirect(
                RHIBufferSpan{gpu_ibuf, 0, 0},
                cmd_span, (uint32_t)data.draw_indirect_commands.size());
            RDGCommandHelper::EndGraphicsRender(queue);
        }
    );

    // Indirect command buffer read.
    raster_pass->AddBufferH(data.d_draw_commands.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead);
}

MI_NAMESPACE_END
