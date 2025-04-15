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

void Renderer::Render(RendererView * view, RenderGraphBuilder & builder) {
    mi_assert(view->view_index_ == 0, "Only one view is supported for now");
    // Remove renderables with ref count approaching 1
    std::vector<TRef<Renderable>> active_renderables;
    active_renderables.reserve(renderables_.size());
    for (auto & e : renderables_) {
        if (e.GetRefCount() == 1) {
            e.SafeRelease();
        } else {
            active_renderables.push_back(e);
        }
    }
    renderables_ = std::move(active_renderables);
    // Update dirty renderables
    for (auto & e : renderables_) {
        if (e->IsDirty()) {
            e->Update(builder);
            e->SetDirty(false);
        }
    }

    std::vector<RHIDrawIndexedIndirectCommand> indirect_commands;
    struct DrawHeader {
        int material_index;
        size_t vertex_buffer_offset;
        size_t index_buffer_offset;
        RHIBuffer * vertex_buffer;
        RHIBuffer * index_buffer;
    };
    std::vector<DrawHeader> draw_headers;
    {
        for (auto & e : renderables_) {
            if (auto mesh = e->As<StaticMesh>()) {
                for (auto [geom, mat] : std::views::zip(mesh->GetGeometries(), mesh->GetMaterials())) {
                    RHIDrawIndexedIndirectCommand cmd {};
                    cmd.first_instance = 0;
                    cmd.first_index = geom->GetDeviceFirstIndex();
                    cmd.instance_count = 1;
                    cmd.index_count = geom->GetIndexCount();
                    // Padding 0 is used for material index
                    cmd.padding0 = mat->GetIndex();
                    indirect_commands.push_back(cmd);
                    DrawHeader header {};
                    header.material_index = mat->GetIndex();
                    header.vertex_buffer_offset = geom->GetDeviceVertexBufferOffset();
                    header.index_buffer_offset = geom->GetDeviceIndexBufferOffset();
                    header.vertex_buffer = geom->GetDeviceVertexBuffer();
                    header.index_buffer = geom->GetDeviceIndexBuffer();
                    draw_headers.push_back(header);
                }
            }
        }
        auto size = sizeof(RHIDrawIndexedIndirectCommand) * indirect_commands.size();
        view->static_mesh_draw_commands_ = RDGBuffer::Create(RHIBufferUsageFlagBits::kIndirect, size);

        Helpers::UploadWithRDG(builder, view->static_mesh_draw_commands_.Raw(), indirect_commands.data(), size);

    }
    // Add a pass to rasterize static meshes
    auto raster_pass = builder.AddPass("RasterizeStaticMeshCommands", RDGPassType::kGraphics, {}, nullptr, nullptr,
        [indirect_commands, draw_headers, rdg_draw_cmd = view->static_mesh_draw_commands_.Raw()](RDGPass * pass, RHICommandQueueGraphics & queue) {
            RHIBuffer * last_vertex_buffer = nullptr;
            size_t last_vertex_buffer_offset = 0;
            RHIBuffer * last_index_buffer = nullptr;
            size_t last_index_buffer_offset = 0;
            RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
            // TODO sort commands first to minimize draw calls
            for (int i = 0; i < (int)indirect_commands.size(); i++) {
                auto & cmd = indirect_commands[i];
                auto & hdr = draw_headers[i];
                if (last_vertex_buffer != hdr.vertex_buffer || last_vertex_buffer_offset != hdr.vertex_buffer_offset
                || last_index_buffer != hdr.index_buffer || last_index_buffer_offset != hdr.index_buffer_offset) {
                    if (i > 0) {
                        // Batch submit previous commands sharing the same vertex & index buffer settings.
                        auto first_cmd = cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand);
                        queue.DrawIndexedIndirect(
                            RHIBufferSpan(hdr.index_buffer, hdr.index_buffer_offset, hdr.index_buffer->GetBufferSize()),
                            cmd_span,  i - first_cmd
                        );
                        cmd_span.offset = i * sizeof(RHIDrawIndexedIndirectCommand);
                    }
                    last_vertex_buffer = hdr.vertex_buffer;
                    last_vertex_buffer_offset = hdr.vertex_buffer_offset;
                    last_index_buffer = hdr.index_buffer;
                    last_index_buffer_offset = hdr.index_buffer_offset;
                }
            }
        }
    );

    {
        // TODO make vertex / index buffers allocated from a pool. So that we can control the number of barriers
        std::set<RHIBuffer*> barrier_buffers;
        for (auto & e : renderables_) {
            if (!e->IsDirty()) continue ;
            if (auto mesh = e->As<StaticMesh>()) {
                for (auto geom : mesh->GetGeometries()) {
                    if (auto vb = geom->GetDeviceVertexBuffer()) barrier_buffers.insert(vb);
                    if (auto ib = geom->GetDeviceIndexBuffer()) barrier_buffers.insert(ib);
                }
            }
        }
        for (auto e : barrier_buffers) {
            // Destructors of temporaries created in one line of code will destruct after the line
            raster_pass->AddBuffer(RDGBuffer::Import(e, RHIGPUAccessFlagBits::kAll).Raw(), RHIGPUAccessFlagBits::kRead);
        }
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

MI_NAMESPACE_END