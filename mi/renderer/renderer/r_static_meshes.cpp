/*
 * Created: 2025/5/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>
#include <renderer/mi_renderer.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>

#include "r_view_common.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_static_mesh.h"

MI_NAMESPACE_BEGIN
class DrawStaticMeshesShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaders)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransforms)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableNormalTransforms)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableIndexAndMaterialIndex)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaders)

        SHADER_RESOURCE_PARAMETER(SamplerState, Sampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointSampler)

        SHADER_VERTEX_BUFFER(sizeof(DefaultStaticMeshVertex), VertexBuffer)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Position), RHIVertexAttributeFormatType::k3xFp32, position)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Normal), RHIVertexAttributeFormatType::k3xFp32, normal)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, UV), RHIVertexAttributeFormatType::k2xFp32, uv)

        SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Albedo)
        SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Normal)
        SHADER_RENDER_TARGET(PixelFormatType::kR8G8_UNORM, MetallicRoughness)
        SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig config {};
        // Reversed-z depth buffer
        config.depth_compare_op = RHIDepthCompareOpType::kGreater;
        return config;
    }
};

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawStaticMeshesShader, "mi/renderer/shaders/DrawStaticMeshes.hlsl", "VS_Main", "PS_Main");

void Renderer::Render_PrepareStaticMeshes (RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
        // Generate draw commands

    auto & data = ctx.static_meshes;

    for (auto & e : ctx.visible_renderables) {
        if (auto mesh_instance = e->As<StaticMeshInstance>()) {
            auto mesh = mesh_instance->GetStaticMesh();
            for (auto [geom, mat] : std::views::zip(mesh->GetGeometries(), mesh->GetMaterials())) {
                auto dev = geom->GetDeviceGeometry();
                RHIDrawIndexedIndirectCommand cmd {};
                cmd.first_instance = 0;
                cmd.first_index = dev->GetDeviceFirstIndex();
                cmd.instance_count = 1;
                cmd.index_count = geom->GetIndexCount();

                DrawInvocationSortingHeader header {};

                header.material_index = mat->GetDeviceMaterial()->GetIndex();
                header.world_renderable_handle = e->GetIndex();

                header.vertex_buffer = dev->GetDeviceVertexBuffer()->GetRHI();
                header.index_buffer = dev->GetDeviceIndexBuffer()->GetRHI();
                header.indirect_command = cmd;

                data.draw_invocation_sorting_headers.push_back(header);
            }
        }
    }


    // Draw commands for static meshes
    data.d_static_draw_commands = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kIndirect,
        sizeof(RHIDrawIndexedIndirectCommand) * data.draw_invocation_sorting_headers.size()
    );
    data.d_static_draw_commands->SetName("StaticMeshDrawCommandsBuffer");
    // Used to index the renderable & material for draw commands, used for viewport rasterization
    data.d_static_mesh_draw_command_renderable_material_indices = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        sizeof(uint32_t) * 2 * data.draw_invocation_sorting_headers.size()
    );

    {
        // Sort the headers, batch draw calls with the same vertex & index buffer
        std::sort(data.draw_invocation_sorting_headers.begin(), data.draw_invocation_sorting_headers.end(),
            [](const DrawInvocationSortingHeader & a, const DrawInvocationSortingHeader & b) {
                if (a.vertex_buffer.buffer != b.vertex_buffer.buffer) return a.vertex_buffer.buffer < b.vertex_buffer.buffer;
                return a.index_buffer.buffer < b.index_buffer.buffer;
            }
        );
        // Generate and upload indirect commands & extra buffers for draw
        {
            data.draw_indirect_commands.reserve(data.draw_invocation_sorting_headers.size());
            auto draw_indirect_renderable_and_material_indices = (uint32_t*)view->temp_allocator_.Allocate(data.draw_invocation_sorting_headers.size() * sizeof(uint32_t) * 2);
            for (auto [i, e] : std::views::enumerate(data.draw_invocation_sorting_headers)) {
                data.draw_indirect_commands.push_back(e.indirect_command);
                draw_indirect_renderable_and_material_indices[i * 2] = e.world_renderable_handle;
                draw_indirect_renderable_and_material_indices[i * 2 + 1] = e.material_index;
            }

            auto size = sizeof(RHIDrawIndexedIndirectCommand) * data.draw_indirect_commands.size();
            // Upload the draw commands
            // no need for temporary memory allocation, because draw_indirect_commands is freed after UploadContext::Fire()
            view->upload_context_.Add(data.d_static_draw_commands.Raw(), data.draw_indirect_commands.data(), size);
            // Upload the renderable & material indices
            view->upload_context_.Add(data.d_static_mesh_draw_command_renderable_material_indices.Raw(),
                draw_indirect_renderable_and_material_indices, data.draw_indirect_commands.size() * sizeof(uint32_t) * 2);
        }
    }
}

void Renderer::Render_DrawStaticMeshes(RendererView *view, RenderGraphBuilder &builder) {

    auto params = builder.Allocate<DrawStaticMeshesShader::Params>();
    params->View = view->view_common_params_;
    params->RenderableHeaders = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->RenderableTransforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    params->RenderableNormalTransforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_normal_transforms_.Raw());
    params->RenderableIndexAndMaterialIndex = ctx.static_meshes.d_static_mesh_draw_command_renderable_material_indices.Raw();
    params->MaterialHeaders = builder.Import(device_allocator_->material_header_buffer_.Raw());
    params->Sampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    params->PointSampler = RHI::Get().GetGlobalSamplers().point_wrap;

    params->Albedo = view->G_albedo_.Raw();
    params->Normal = view->G_normal_.Raw();
    params->MetallicRoughness = view->G_metallic_roughness_.Raw();
    params->Depth = view->G_depth_.Raw();

    auto shader = RDGShaderLibrary::Get().GetShader<DrawStaticMeshesShader>();

    // Rasterize static meshes with batched drawing
    auto raster_pass = builder.AddPass<DrawStaticMeshesShader>({}, params,
        [params, shader, data = ctx.static_meshes, rdg_draw_cmd = ctx.static_meshes.d_static_draw_commands.Raw()]
        ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            if (RDGCommandHelper::BindGraphicsShader<DrawStaticMeshesShader>(
                queue, pass, shader, params, true
            )) {
                queue.BeginRendering();
                RHIBufferSpan last_vertex_buffer {};
                RHIBufferSpan last_index_buffer {};
                RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
                for (int i = 0; i < (int)data.draw_indirect_commands.size(); i++) {
                    // auto & cmd = indirect_commands[i];
                    auto & hdr = data.draw_invocation_sorting_headers[i];
                    if (last_vertex_buffer != hdr.vertex_buffer || last_index_buffer != hdr.index_buffer) {
                        if (i > 0) {
                            // Batch submit previous commands sharing the same vertex & index buffer settings.
                            auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                            queue.DrawIndexedIndirect(
                                data.draw_invocation_sorting_headers[i-1].index_buffer,
                                cmd_span,  i - first_cmd
                            );
                            cmd_span.offset = i * sizeof(RHIDrawIndexedIndirectCommand);
                        }
                        last_vertex_buffer = hdr.vertex_buffer;
                        last_index_buffer = hdr.index_buffer;
                        queue.BindVertexBuffer(0, hdr.vertex_buffer);
                    }
                }
                // Submit last batch if not empty
                if (!data.draw_indirect_commands.empty()) {
                    int i = (int)data.draw_indirect_commands.size();
                    // Batch submit previous commands sharing the same vertex & index buffer settings.
                    auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                    queue.DrawIndexedIndirect(
                        data.draw_invocation_sorting_headers[i-1].index_buffer,
                        cmd_span,  i - first_cmd);
                }
                queue.EndRendering();
            }
        }
    );

    // Place barriers for geometry / indirect buffers manually
    {
        // TODO utilize DeviceBindlessAllocator, place less barriers.
        std::set<RHIBuffer*> barrier_buffers;
        for (auto & e : ctx.visible_renderables) {
            if (!e->IsDirty()) continue ;
            if (auto mesh_instance = e->As<StaticMeshInstance>()) {
                auto mesh = mesh_instance->GetStaticMesh();
                for (auto geom : mesh->GetGeometries()) {
                    if (auto dev = geom->GetDeviceGeometry()) {
                        if (auto vb = dev->GetDeviceVertexBuffer()) barrier_buffers.insert(vb->GetRHI().buffer);
                        if (auto ib = dev->GetDeviceIndexBuffer()) barrier_buffers.insert(ib->GetRHI().buffer);
                    }
                }
            }
        }
        for (auto e : barrier_buffers) {
            // Destructors of temporaries created in one line of code will destruct after the line
            raster_pass->AddBuffer(
                builder.Import(e, RHIGPUAccessFlagBits::kTransferWrite, RHIPipelineStageFlagBits::kTransfer),
                RHIGPUAccessFlagBits::kVertexAttributeRead | RHIGPUAccessFlagBits::kIndexRead
            );
        }

        // Indirect command
        raster_pass->AddBuffer(ctx.static_meshes.d_static_draw_commands.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead);
    }
}


MI_NAMESPACE_END