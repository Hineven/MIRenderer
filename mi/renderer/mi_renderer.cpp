/*
 * Created: 2025/4/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>

#include "renderer/mi_renderer.h"

#include <barrier>
#include <core/infra.h>
#include <rdg/rdg_builder.h>
#include <renderer/mi_static_mesh.h>
#include <renderer/mi_renderer_view.h>
#include <renderer/mi_helpers.h>
#include <renderer/mi_material.h>
#include <rhi/rhi_buffer.h>
#include <vulkan/vulkan_structs.hpp>

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

void Renderer::Init() {

}

void Renderer::UpdateView(RendererView *view) {
    view->output_ = RDGTexture::Import(view->output_->GetRHI());
}


void Renderer::Render(RendererView * view, RenderGraphBuilder & builder) {

    UpdateView(view);

    if (!view->world) {
        MI_WARN("World is not present in the view.");
        return ;
    }
    mi_assert(view->view_index_ == 0, "Only one view is supported for now");
    // Remove renderables with ref count approaching 1
    std::vector<TRef<Renderable>> active_renderables;
    auto all_renderables = view->world->GetRenderables();
    active_renderables.reserve(all_renderables.size());
    // Update renderable transforms
    {
        std::vector<glm::mat4x3> transforms;
        transforms.reserve(all_renderables.size());
        for (auto & e : all_renderables) {
            transforms.push_back(e->transform_.GetToWorldTransformMatrix());
        }
        Helpers::UploadWithRDG(
            builder, view->world->GetDevice()->renderable_transforms_->GetRHI(),
            transforms.data(), sizeof(glm::mat4x3) * transforms.size()
        );
    }
    // Update dirty renderables with custom logic
    for (auto & e : all_renderables) {
        if (e->IsDirty()) {
            e->Update(builder);
            e->SetDirty(false);
        }
    }

    // Filter visible rendeables
    for (auto & e : all_renderables) {
        if (e->IsVisible()) active_renderables.push_back(e);
    }

    // Generate indirect draw commands
    std::vector<RHIDrawIndexedIndirectCommand> indirect_commands;
    struct DrawHeader {
        int material_index;
        RHIBufferSpan vertex_buffer;
        RHIBufferSpan index_buffer;
    };
    std::vector<DrawHeader> draw_headers;
    {
        for (auto & e : active_renderables) {
            if (auto mesh = e->As<StaticMesh>()) {
                for (auto [geom, mat] : std::views::zip(mesh->GetGeometries(), mesh->GetMaterials())) {
                    auto dev = geom->GetDeviceGeometry();
                    RHIDrawIndexedIndirectCommand cmd {};
                    cmd.first_instance = 0;
                    cmd.first_index = dev->GetDeviceFirstIndex();
                    cmd.instance_count = 1;
                    cmd.index_count = geom->GetIndexCount();
                    // Padding 0 is used for material index
                    cmd.padding0 = mat->GetIndex();
                    indirect_commands.push_back(cmd);
                    DrawHeader header {};
                    header.material_index = mat->GetIndex();
                    header.vertex_buffer = dev->GetDeviceVertexBuffer();
                    header.index_buffer = dev->GetDeviceIndexBuffer();
                    draw_headers.push_back(header);
                }
            }
        }
        auto size = sizeof(RHIDrawIndexedIndirectCommand) * indirect_commands.size();
        view->static_mesh_draw_commands_ = RDGBuffer::Create(RHIBufferUsageFlagBits::kIndirect, size);

        Helpers::UploadWithRDG(builder, view->static_mesh_draw_commands_->GetRHI(), indirect_commands.data(), size);

    }

    // Rasterize static meshes with batched drawing
    // manual barrier placement
    auto raster_pass = builder.AddPass("RasterizeStaticMeshCommands", RDGPassType::kGraphics, {}, nullptr, nullptr,
        [indirect_commands, draw_headers, rdg_draw_cmd = view->static_mesh_draw_commands_.Raw()](RDGPass * pass, RHICommandQueueGraphics & queue) {
            RHIBufferSpan last_vertex_buffer {};
            RHIBufferSpan last_index_buffer {};
            RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
            // TODO sort commands first to minimize draw calls
            for (int i = 0; i < (int)indirect_commands.size(); i++) {
                // auto & cmd = indirect_commands[i];
                auto & hdr = draw_headers[i];
                if (last_vertex_buffer != hdr.vertex_buffer || last_index_buffer != hdr.index_buffer) {
                    if (i > 0) {
                        // Batch submit previous commands sharing the same vertex & index buffer settings.
                        auto first_cmd = cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand);
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
        // TODO make vertex / index buffers allocated from a pool. So that we can control the number of barriers
        std::set<RHIBuffer*> barrier_buffers;
        for (auto & e : active_renderables) {
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
        raster_pass->AddBuffer(view->static_mesh_draw_commands_.Raw(), RHIGPUAccessFlagBits::kRead);
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

    // Draw G-Buffer to output directly for debug purposes
    Render_DrawToOutput(view, builder, view->G_albedo_.Raw());
}

MI_NAMESPACE_END