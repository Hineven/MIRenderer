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

#include "rdg/rdg_cmd.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/r_ctx.h"

MI_NAMESPACE_BEGIN

Renderer::Renderer() {

}

Renderer::~Renderer() {

}


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

void Renderer::Init(CommonGroupedDeviceResourceAllocator * allocator, RDGResourcePool * pool) {
    device_allocator_ = allocator;
    pool_ = pool;
}

void Renderer::FrameContext::Init() {

}


void Renderer::FrameContext::Deinit() {
    visible_renderables.clear();
    static_meshes = {};
}


void Renderer::Render(RendererView * view, RenderGraphBuilder & builder) {

    view->InitFrame();

    // Allocate and set view->view_common_params_
    view->SetViewCommonShaderParameters(builder);

    // Init frame context
    ctx.Init();

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
    // Upload renderable transforms and headers
    view->upload_context_.Add(
        builder.Import(view->world_->d_renderable_transforms_.Raw()),
        renderable_transforms.data(),
        renderable_transforms.size() * sizeof(glm::mat4x3));
    view->upload_context_.Add(
        builder.Import(view->world_->d_renderable_headers_.Raw()),
        renderable_headers.data(),
        renderable_headers.size() * sizeof(RenderableHeader)
    );


    // Filter visible rendeables
    ctx.visible_renderables.reserve(all_renderables.size());
    for (auto & e : all_renderables) {
        if (e->IsVisible()) ctx.visible_renderables.push_back(e);
    }

    // Prepare static mesh draw commands
    Render_PrepareStaticMeshes(view, builder);

    // Fire batched uploads to the RDG
    view->upload_context_.Fire(builder);

    // Ready for rendering

    // Draw the sky first.
    Render_DrawSky(view, builder);

    // Clear Depth buffer
    builder.AddPass("ClearDepth", RDGPassFlagBits::kNeverCull,
        [depth = view->G_depth_.Raw()](RDGPass * pass, RHICommandQueueGraphics & queue) {
        queue.ClearTexture(depth->GetRHI(), {1, 1, 1, 1});
    })->AddTexture(view->G_depth_.Raw(), RDGTextureUsageType::kTransferDst);

    Render_DrawStaticMeshes(view, builder);

    // Draw G-Buffer to output directly for debug purposes
    Render_DrawToOutput(view, builder, view->G_albedo_.Raw());


    // Reset frame context
    ctx.Deinit();

    // Update persistent data using current frame for next frame use
    view->UpdatePersistentData();

}

MI_NAMESPACE_END