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
#include "rdg/rdg_helper.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_static_mesh.h"
#include "rhi/rhi_buffer.h"

MI_NAMESPACE_BEGIN
class DrawDeferredStaticMeshesShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableNormalTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableIndexAndDescriptorIndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)

        SHADER_VERTEX_BUFFER(sizeof(DefaultStaticMeshVertex), VertexBuffer)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Position), RHIVertexAttributeFormatType::k3xFp32, position)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Normal), RHIVertexAttributeFormatType::k3xFp32, normal)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, UV), RHIVertexAttributeFormatType::k2xFp32, uv)

        SHADER_RENDER_TARGET(PixelFormatType::kR32G32B32A32_UINT, Visibility, {})
        SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth, {})
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

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawDeferredStaticMeshesShader, "mi/renderer/shaders/DrawStaticMeshes.hlsl", "DrawDeferredStaticMeshesVS", "DrawDeferredStaticMeshesPS");

void Renderer::Render_PrepareStaticMeshes (RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    // Generate draw commands
    auto SpawnDrawData = [&] (FrameContext::StaticMeshes & data, bool forward) {
        for (auto & e : ctx.visible_renderables) {
            if (auto mesh_instance = e->As<StaticMeshInstance>()) {
                auto mesh = mesh_instance->GetStaticMesh();
                for (const auto& [descriptor_index, tup] : std::views::zip(mesh->GetGeometries(), mesh->GetMaterials()) | std::views::enumerate) {
                    auto geom = std::get<0>(tup);
                    auto mat = std::get<1>(tup);
                    if (forward != mat->IsForward()) continue;
                    auto dev = geom->GetDeviceGeometry();
                    RHIDrawIndexedIndirectCommand cmd {};
                    cmd.first_instance = 0; // Filled after sorting
                    // The offset within its index uber buffer
                    cmd.first_index = (uint32_t)(dev->GetDeviceIndexBuffer()->GetOffset() / sizeof(uint32_t));
                    // The offset within its vertex uber buffer
                    cmd.vertex_offset = (uint32_t)(dev->GetDeviceVertexBuffer()->GetOffset() / sizeof(DefaultStaticMeshVertex));
                    cmd.instance_count = 1;
                    cmd.index_count = geom->GetIndexCount();

                    DrawInvocationSortingHeader header {};

                    header.descriptor_index = (uint)descriptor_index;
                    header.world_renderable_handle = e->GetIndex();

                    header.vertex_buffer = dev->GetDeviceVertexBuffer()->GetRHI().buffer;
                    header.index_buffer = dev->GetDeviceIndexBuffer()->GetRHI().buffer;
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
        data.d_static_mesh_draw_command_renderable_descriptor_indices = RDGBuffer::Create(
            RHIBufferUsageFlagBits::kStorage,
            sizeof(uint32_t) * 2 * data.draw_invocation_sorting_headers.size()
        );

        {
            // Sort the headers, batch draw calls with the same vertex & index buffer
            std::sort(data.draw_invocation_sorting_headers.begin(), data.draw_invocation_sorting_headers.end(),
                [](const DrawInvocationSortingHeader & a, const DrawInvocationSortingHeader & b) {
                    if (a.vertex_buffer != b.vertex_buffer) return a.vertex_buffer < b.vertex_buffer;
                    return a.index_buffer < b.index_buffer;
                }
            );
            // Generate and upload indirect commands & extra buffers for draw
            {
                data.draw_indirect_commands.reserve(data.draw_invocation_sorting_headers.size());
                auto draw_indirect_renderable_and_descriptor_indices = (uint32_t*)view->temp_allocator_.Allocate(data.draw_invocation_sorting_headers.size() * sizeof(uint32_t) * 2);
                for (auto [i, e] : std::views::enumerate(data.draw_invocation_sorting_headers)) {
                    // Fill the first instance of each draw
                    e.indirect_command.first_instance = (uint32_t)i;
                    data.draw_indirect_commands.push_back(e.indirect_command);
                    draw_indirect_renderable_and_descriptor_indices[i * 2] = e.world_renderable_handle;
                    draw_indirect_renderable_and_descriptor_indices[i * 2 + 1] = e.descriptor_index;
                }

                auto size = sizeof(RHIDrawIndexedIndirectCommand) * data.draw_indirect_commands.size();
                // Upload the draw commands
                // no need for temporary memory allocation, because draw_indirect_commands is freed after UploadContext::Fire()
                view->upload_context_.Add(data.d_static_draw_commands.Raw(), data.draw_indirect_commands.data(), size);
                // Upload the renderable & material indices
                view->upload_context_.Add(data.d_static_mesh_draw_command_renderable_descriptor_indices.Raw(),
                    draw_indirect_renderable_and_descriptor_indices, data.draw_indirect_commands.size() * sizeof(uint32_t) * 2);
            }
        }
    };
    SpawnDrawData(ctx.deferred_static_meshes, false);
    SpawnDrawData(ctx.forward_static_meshes, true);
}

class DecodeVisibilityShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableNormalTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)

        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)

        SHADER_RESOURCE_PARAMETER(Texture2D, VisibilityTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWAlbedo)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWNormal)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWEmission)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWMetallicRoughness)
        SHADER_RESOURCE_PARAMETER(Texture2D, DepthTexture)
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

IMPLEMENT_RDG_COMPUTE_SHADER(DecodeVisibilityShader, "mi/renderer/shaders/DrawStaticMeshes.hlsl", "DecodeVisibility");

void Renderer::Render_DrawDeferredStaticMeshes(RendererView *view, RenderGraphBuilder &builder) {
    RDGSectionGuard section_guard(builder, "Render_DrawDeferredStaticMeshes");
    {   
        auto params = builder.Allocate<DrawDeferredStaticMeshesShader::Params>();
        params->View = view->view_common_params_;
        params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
        params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
        params->RenderableNormalTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_normal_transforms_.Raw());
        params->RenderableIndexAndDescriptorIndexBuffer = ctx.deferred_static_meshes.d_static_mesh_draw_command_renderable_descriptor_indices.Raw();
        params->MaterialHeaderBuffer = builder.Import(device_allocator_->material_header_buffer_.Raw());

        params->Visibility = view->G_visibility_.Raw();
        params->Visibility.load_op = RHILoadOpType::kClear;
        params->Visibility.clear_value = {std::bit_cast<float>(0xffffffffu), std::bit_cast<float>(0xffffffffu), 0, 0};
        params->Depth = view->G_depth_.Raw();
        params->Depth.load_op = RHILoadOpType::kClear;
        params->Depth.clear_value = {0.0f, 0};
        auto shader = RDGShaderLibrary::Get().GetShader<DrawDeferredStaticMeshesShader>();

        // Rasterize static meshes with batched drawing
        auto raster_pass = builder.AddPass<DrawDeferredStaticMeshesShader>({}, shader, params,
            [params, shader, data = ctx.deferred_static_meshes, rdg_draw_cmd = ctx.deferred_static_meshes.d_static_draw_commands.Raw()]
            ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
                if (auto ctx = RDGCommandHelper::BindGraphicsShader<DrawDeferredStaticMeshesShader>(
                    queue, pass, shader, params, true
                )) {
                    queue.BeginRendering();
                    queue.SetCullMode(RHICullModeType::kBack);
                    RHIBuffer * last_vertex_buffer {};
                    RHIBuffer * last_index_buffer {};
                    RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
                    for (int i = 0; i < (int)data.draw_indirect_commands.size(); i++) {
                        auto & hdr = data.draw_invocation_sorting_headers[i];
                        if (last_vertex_buffer != hdr.vertex_buffer || last_index_buffer != hdr.index_buffer) {
                            if (i > 0) {
                                // Batch submit previous commands sharing the same vertex & index buffer settings.
                                auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                                queue.DrawIndexedIndirect(
                                    data.draw_invocation_sorting_headers[i-1].index_buffer->GetSpan(),
                                    cmd_span,  i - first_cmd
                                );
                                cmd_span.offset = i * sizeof(RHIDrawIndexedIndirectCommand);
                            }
                            last_vertex_buffer = hdr.vertex_buffer;
                            last_index_buffer = hdr.index_buffer;
                            queue.BindVertexBuffer(0, hdr.vertex_buffer->GetSpan());
                        }
                    }
                    // Submit last batch if not empty
                    if (!data.draw_indirect_commands.empty()) {
                        int i = (int)data.draw_indirect_commands.size();
                        // Batch submit previous commands sharing the same vertex & index buffer settings.
                        auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                        queue.DrawIndexedIndirect(
                            data.draw_invocation_sorting_headers[i-1].index_buffer->GetSpan(),
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
                raster_pass->AddBufferH(
                    builder.Import(e, RHIGPUAccessFlagBits::kTransferWrite, RHIPipelineStageFlagBits::kTransfer),
                    RHIGPUAccessFlagBits::kVertexAttributeRead | RHIGPUAccessFlagBits::kIndexRead
                );
            }

            // Indirect command
            raster_pass->AddBufferH(ctx.deferred_static_meshes.d_static_draw_commands.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead);
        }
    }
    // Decode G-Buffers from visibility
    {
        auto shader = RDGShaderLibrary::Get().GetShader<DecodeVisibilityShader>();
        auto tiles_x = DivideAndRoundUp(view->film_width_, DecodeVisibilityShader::kTileSize);
        auto tiles_y = DivideAndRoundUp(view->film_height_, DecodeVisibilityShader::kTileSize);
        auto params = builder.Allocate<DecodeVisibilityShader::Params>();
        {
            params->View = view->view_common_params_;
            params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
            params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
            params->RenderableNormalTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_normal_transforms_.Raw());
            params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->static_mesh_description_uber_buffer_->GetRHI());
            params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->static_mesh_header_buffer_.Raw());
            params->GeometryHeaderBuffer = builder.Import(device_allocator_->geometry_header_buffer_.Raw());
            params->MaterialHeaderBuffer = builder.Import(device_allocator_->material_header_buffer_.Raw());
            params->IndexBuffer = builder.Import(device_allocator_->index_uber_buffer_->GetRHI());
            params->VertexBuffer = builder.Import(device_allocator_->vertex_uber_buffer_->GetRHI());
            params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
            params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;
            params->RWAlbedo = view->G_albedo_.Raw();
            params->RWNormal = view->G_normal_.Raw();
            params->RWEmission = view->G_emission_.Raw();
            params->RWMetallicRoughness = view->G_metallic_roughness_.Raw();
            params->VisibilityTexture = view->G_visibility_.Raw();
            params->DepthTexture = view->G_depth_.Raw();
        }
        Helpers::AddComputePass(builder, shader, params, tiles_x, tiles_y);
    }
}

class DrawForwardStaticMeshesShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableNormalTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableIndexAndDescriptorIndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)

        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)

        SHADER_VERTEX_BUFFER(sizeof(DefaultStaticMeshVertex), VertexBuffer)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Position), RHIVertexAttributeFormatType::k3xFp32, position)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Normal), RHIVertexAttributeFormatType::k3xFp32, normal)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, UV), RHIVertexAttributeFormatType::k2xFp32, uv)

        SHADER_RENDER_TARGET(PixelFormatType::kR32G32B32A32_UINT, Visibility, {})
        SHADER_RENDER_TARGET(PixelFormatType::kB8G8R8A8_SRGB, Color, RDGShaderRenderTargetBlendingSettings{
            .blend_op = RHIBlendOpType::kBlendAdd,
            .src_blend = RHIBlendFactorType::kSrcAlpha,
            .dst_blend = RHIBlendFactorType::kOneMinusSrcAlpha
        })
        SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth, {})
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

IMPLEMENT_RDG_GRAPHICS_SHADER(
    DrawForwardStaticMeshesShader,
    "mi/renderer/shaders/DrawStaticMeshes.hlsl",
    "DrawForwardStaticMeshesVS", "DrawForwardStaticMeshesPS"
);

void Renderer::Render_DrawForwardStaticMeshes(RendererView *view, RenderGraphBuilder &builder) {
    RDGSectionGuard section_guard(builder, "Render_DrawForwardStaticMeshes");
    Helpers::Clear(builder, view->forward_depth_.Raw(), {});

    {
        auto params = builder.Allocate<DrawForwardStaticMeshesShader::Params>();
        params->View = view->view_common_params_;
        params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
        params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
        params->RenderableNormalTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_normal_transforms_.Raw());
        params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->static_mesh_description_uber_buffer_->GetRHI());
        params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->static_mesh_header_buffer_.Raw());
        params->RenderableIndexAndDescriptorIndexBuffer = ctx.forward_static_meshes.d_static_mesh_draw_command_renderable_descriptor_indices.Raw();
        params->MaterialHeaderBuffer = builder.Import(device_allocator_->material_header_buffer_.Raw());

        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
        params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;

        params->Visibility = view->G_visibility_.Raw();
        params->Color = builder.Import(RHI::Get().GetBackBuffer());
        params->Color.load_op = RHILoadOpType::kLoad;
        params->Depth = view->forward_depth_.Raw();
        auto shader = RDGShaderLibrary::Get().GetShader<DrawForwardStaticMeshesShader>();

        // Rasterize static meshes with batched drawing
        auto raster_pass = builder.AddPass<DrawForwardStaticMeshesShader>({}, shader, params,
            [params, shader, data = ctx.forward_static_meshes, rdg_draw_cmd = ctx.forward_static_meshes.d_static_draw_commands.Raw()]
            ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
                if (auto ctx = RDGCommandHelper::BindGraphicsShader<DrawForwardStaticMeshesShader>(
                    queue, pass, shader, params, true
                )) {
                    queue.BeginRendering();
                    queue.SetCullMode(RHICullModeType::kBack);
                    RHIBuffer * last_vertex_buffer {};
                    RHIBuffer * last_index_buffer {};
                    RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
                    for (int i = 0; i < (int)data.draw_indirect_commands.size(); i++) {
                        auto & hdr = data.draw_invocation_sorting_headers[i];
                        if (last_vertex_buffer != hdr.vertex_buffer || last_index_buffer != hdr.index_buffer) {
                            if (i > 0) {
                                // Batch submit previous commands sharing the same vertex & index buffer settings.
                                auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                                queue.DrawIndexedIndirect(
                                    data.draw_invocation_sorting_headers[i-1].index_buffer->GetSpan(),
                                    cmd_span,  i - first_cmd
                                );
                                cmd_span.offset = i * sizeof(RHIDrawIndexedIndirectCommand);
                            }
                            last_vertex_buffer = hdr.vertex_buffer;
                            last_index_buffer = hdr.index_buffer;
                            queue.BindVertexBuffer(0, hdr.vertex_buffer->GetSpan());
                        }
                    }
                    // Submit last batch if not empty
                    if (!data.draw_indirect_commands.empty()) {
                        int i = (int)data.draw_indirect_commands.size();
                        // Batch submit previous commands sharing the same vertex & index buffer settings.
                        auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                        queue.DrawIndexedIndirect(
                            data.draw_invocation_sorting_headers[i-1].index_buffer->GetSpan(),
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
                raster_pass->AddBufferH(
                    builder.Import(e, RHIGPUAccessFlagBits::kTransferWrite, RHIPipelineStageFlagBits::kTransfer),
                    RHIGPUAccessFlagBits::kVertexAttributeRead | RHIGPUAccessFlagBits::kIndexRead
                );
            }

            // Indirect command
            raster_pass->AddBufferH(ctx.forward_static_meshes.d_static_draw_commands.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead);
        }
    }
}



MI_NAMESPACE_END