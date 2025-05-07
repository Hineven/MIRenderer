/*
 * Created: 2025/4/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>

#include "shaders/shared/SharedRenderable.hlsl"
#include "renderer/mi_renderer.h"

#include <barrier>
#include <core/infra.h>
#include <rdg/rdg_builder.h>
#include <renderer/mi_static_mesh.h>
#include <renderer/mi_renderer_view.h>
#include <renderer/mi_helpers.h>
#include <renderer/mi_material.h>
#include <rhi/rhi_buffer.h>

MI_NAMESPACE_BEGIN

static Renderer * g_renderer = nullptr;

Renderer *Renderer::GetPointer() {
    return g_renderer;
}


Renderer &Renderer::Get() {
    if (!g_renderer) g_renderer = new Renderer();
    return * g_renderer;
}

void Renderer::DestroySingleton() {
    if (g_renderer) {
        delete g_renderer;
        g_renderer = nullptr;
    }
}

void Renderer::Init(RDGResourcePool * pool) {
    pool_ = pool;
}

void Renderer::Render(RendererView * view, RenderGraphBuilder & builder) {

    view->InitFrame();

    if (!view->world_) {
        MI_WARN("World is not present in the view.");
        return ;
    }
    mi_assert(view->persistent_data_->view_index == 0, "Only one view is supported for now");
    auto all_renderables = view->world_->GetRenderables();

    // Update dirty renderables with custom logic
    for (auto & e : all_renderables) {
        if (e->IsDirty()) {
            e->Update(view, builder);
            e->SetDirty(false);
        }
    }

    // Gather renderable common data for upload
    std::vector<glm::mat4x3> renderable_transforms;
    std::vector<RenderableHeader> renderable_headers;
    {
        renderable_transforms.reserve(all_renderables.size());
        renderable_headers.reserve(all_renderables.size());
        for (auto & e : all_renderables) {
            renderable_transforms.push_back(e->GetTransform().GetToWorldTransformMatrix());
            renderable_headers.push_back(e->GetDeviceRenderableHeader());
        }
    }

    std::vector<TRef<Renderable>> visible_renderables;
    visible_renderables.reserve(all_renderables.size());
    // Filter visible rendeables
    for (auto & e : all_renderables) {
        if (e->IsVisible()) visible_renderables.push_back(e);
    }

    {

        // Generate draw commands
        struct DrawInvocationSortingHeader {
            uint32_t material_index;
            uint32_t world_renderable_handle;
            RHIBufferSpan vertex_buffer;
            RHIBufferSpan index_buffer;
            RHIDrawIndexedIndirectCommand indirect_command;
        };
        std::vector<DrawInvocationSortingHeader> draw_invocation_sorting_headers;
        std::vector<RHIDrawIndexedIndirectCommand> draw_indirect_commands;

        for (auto & e : visible_renderables) {
            if (auto mesh = e->As<StaticMesh>()) {
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

                    header.vertex_buffer = dev->GetDeviceVertexBuffer();
                    header.index_buffer = dev->GetDeviceIndexBuffer();
                    header.indirect_command = cmd;

                    draw_invocation_sorting_headers.push_back(header);
                }
            }
        }


        // Draw commands for static meshes
        auto d_static_draw_commands = RDGBuffer::Create(RHIBufferUsageFlagBits::kIndirect, sizeof(RHIDrawIndexedIndirectCommand) * draw_invocation_sorting_headers.size());
        // Used to index the renderable & material for draw commands, used for viewport rasterization
        auto d_static_mesh_draw_command_renderable_material_indices = RDGBuffer::Create(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * 2 * draw_invocation_sorting_headers.size());

        {
            // Sort the headers, batch draw calls with the same vertex & index buffer
            std::sort(draw_invocation_sorting_headers.begin(), draw_invocation_sorting_headers.end(),
                [](const DrawInvocationSortingHeader & a, const DrawInvocationSortingHeader & b) {
                    if (a.vertex_buffer.buffer != b.vertex_buffer.buffer) return a.vertex_buffer.buffer < b.vertex_buffer.buffer;
                    return a.index_buffer.buffer < b.index_buffer.buffer;
                }
            );
            // Generate and upload indirect commands & extra buffers for draw
            {
                draw_indirect_commands.reserve(draw_invocation_sorting_headers.size());
                auto draw_indirect_renderable_and_material_indices = (uint32_t*)view->temp_allocator_.Allocate(draw_invocation_sorting_headers.size() * sizeof(uint32_t) * 2);
                for (auto [i, e] : std::views::enumerate(draw_invocation_sorting_headers)) {
                    draw_indirect_commands.push_back(e.indirect_command);
                    draw_indirect_renderable_and_material_indices[i * 2] = e.world_renderable_handle;
                    draw_indirect_renderable_and_material_indices[i * 2 + 1] = e.material_index;
                }

                auto size = sizeof(RHIDrawIndexedIndirectCommand) * draw_indirect_commands.size();
                // Upload the draw commands
                // no need for temporary memory allocation, because draw_indirect_commands is freed after UploadContext::Fire()
                view->upload_context_.Add(d_static_draw_commands.Raw(), draw_indirect_commands.data(), size);
                // Upload the renderable & material indices
                view->upload_context_.Add(d_static_mesh_draw_command_renderable_material_indices.Raw(),
                    draw_indirect_renderable_and_material_indices, draw_indirect_commands.size() * sizeof(uint32_t));
            }
        }

        // Fire batched uploads to the RDG
        view->upload_context_.Fire(builder);

        // Ready for rendering

        // Draw the sky first
        Render_Sky(view, builder);

        // Rasterize static meshes with batched drawing
        // manual barrier placement
        auto raster_pass = builder.AddPass("RasterizeStaticMeshCommands", RDGPassType::kGraphics, {}, nullptr, nullptr,
            [draw_indirect_commands, draw_invocation_sorting_headers, rdg_draw_cmd = d_static_draw_commands.Raw()]
            ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
                RHIBufferSpan last_vertex_buffer {};
                RHIBufferSpan last_index_buffer {};
                RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
                for (int i = 0; i < (int)draw_indirect_commands.size(); i++) {
                    // auto & cmd = indirect_commands[i];
                    auto & hdr = draw_invocation_sorting_headers[i];
                    if (last_vertex_buffer != hdr.vertex_buffer || last_index_buffer != hdr.index_buffer) {
                        if (i > 0) {
                            // Batch submit previous commands sharing the same vertex & index buffer settings.
                            auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                            queue.DrawIndexedIndirect(hdr.index_buffer, cmd_span,  i - first_cmd);
                            cmd_span.offset = i * sizeof(RHIDrawIndexedIndirectCommand);
                        }
                        last_vertex_buffer = hdr.vertex_buffer;
                        last_index_buffer = hdr.index_buffer;
                        queue.BindVertexBuffer(0, hdr.vertex_buffer);
                    }
                }
            }
        );

        // Place barriers manually
        {
            // TODO utilize CommonGroupedResourceAllocator, place less barriers.
            std::set<RHIBuffer*> barrier_buffers;
            for (auto & e : visible_renderables) {
                if (!e->IsDirty()) continue ;
                if (auto mesh = e->As<StaticMesh>()) {
                    for (auto geom : mesh->GetGeometries()) {
                        if (auto dev = geom->GetDeviceGeometry()) {
                            if (auto vb = dev->GetDeviceVertexBuffer()) barrier_buffers.insert(vb.buffer);
                            if (auto ib = dev->GetDeviceIndexBuffer()) barrier_buffers.insert(ib.buffer);
                        }
                    }
                }
            }
            for (auto e : barrier_buffers) {
                // Destructors of temporaries created in one line of code will destruct after the line
                raster_pass->AddBuffer(RDGBuffer::Import(e, RHIGPUAccessFlagBits::kAll).Raw(), RHIGPUAccessFlagBits::kRead);
            }
            // Indirect command
            raster_pass->AddBuffer(d_static_draw_commands.Raw(), RHIGPUAccessFlagBits::kRead);
            // Renderable & material indices
            raster_pass->AddBuffer(d_static_mesh_draw_command_renderable_material_indices.Raw(), RHIGPUAccessFlagBits::kRead);
            // G-buffers
            raster_pass->AddTexture(
                view->G_depth_.Raw(),
                RDGTextureUsageType::kDepthStencilAttachment
            );
            raster_pass->AddTexture(
                view->G_albedo_.Raw(),
                RDGTextureUsageType::kOutputAttachment
            );
            raster_pass->AddTexture(
                view->G_normal_.Raw(),
                RDGTextureUsageType::kOutputAttachment
            );
            raster_pass->AddTexture(
                view->G_roughness_.Raw(),
                RDGTextureUsageType::kOutputAttachment
            );
        }
    }

    // Draw G-Buffer to output directly for debug purposes
    Render_DrawToOutput(view, builder, view->G_albedo_.Raw());
}

MI_NAMESPACE_END