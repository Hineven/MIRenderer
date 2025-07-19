/*
 * Created: 2025/4/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>

#include "shaders/shared/SharedRenderable.hlsl"

#include <barrier>
#include <core/infra.h>
#include <rhi/rhi_as.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_cmd.h>

#include <renderer/mi_renderer.h>
#include <renderer/mi_static_mesh.h>
#include <renderer/mi_renderer_view.h>
#include <renderer/mi_material.h>


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

void Renderer::Init(DeviceBindlessResourceAllocator * allocator, RDGResourcePool * pool) {
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

    struct RenderFunctionContext {
        Renderer * r_;
        RenderFunctionContext (Renderer * r): r_(r) {
            r_->ctx.Init();
        }
        ~RenderFunctionContext() {
            r_->ctx.Deinit();
        }
    } context_holder(this);

    if (!view->scene_) {
        MI_WARN("World is not present in the view.");
        return ;
    }
    if (!view->scene_->GetDeviceScene()) {
        MI_WARN("Device scene is not present in the world.");
        return ;
    }

    view->InitFrame();
    // Allocate and set view->view_common_params_
    view->SetViewCommonShaderParameters(builder);

    mi_assert(view->persistent_data_->view_index == 0, "Only one view is supported for now");
    auto all_renderables = view->scene_->GetRenderables();

    // Update dirty renderables with custom logic
    for (auto & e : all_renderables) {
        if (e && e->IsDirty()) {
            e->Update(view, builder);
        }
    }

    // Gather renderable common data for upload
    std::vector<glm::mat4x3> renderable_transforms;
    std::vector<glm::mat3x3> renderable_normal_transforms;
    std::vector<RenderableHeader> renderable_headers;
    std::vector<int> visible_renderable_indices;
    {
        renderable_transforms.reserve(all_renderables.size());
        renderable_headers.reserve(all_renderables.size());
        for (auto & e : all_renderables) {
            if (e) {
                auto to_world = e->GetTransform().GetToWorldTransformMatrix();
                renderable_transforms.push_back(to_world);
                auto normal_transform = glm::transpose(glm::inverse(glm::mat3(to_world)));
                renderable_normal_transforms.push_back(normal_transform);
                renderable_headers.push_back(e->GetDeviceRenderableHeader());
                // Clear dirty flag
                e->SetTransformDirty(false);
                if (e->IsVisible()) visible_renderable_indices.push_back(e->GetIndex());
            }
        }
    }
    // Upload renderable transforms and headers
    view->upload_context_.Add(
        builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw(),
            RHIGPUAccessFlagBits::kAll, RHIPipelineStageFlagBits::kAll),
        renderable_transforms.data(),
        renderable_transforms.size() * sizeof(glm::mat4x3));
    view->upload_context_.Add(
        builder.Import(view->scene_->GetDeviceScene()->d_renderable_normal_transforms_.Raw(),
            RHIGPUAccessFlagBits::kAll, RHIPipelineStageFlagBits::kAll),
        renderable_normal_transforms.data(),
        renderable_normal_transforms.size() * sizeof(glm::mat3x3)
    );
    view->upload_context_.Add(
        builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw(),
            RHIGPUAccessFlagBits::kAll, RHIPipelineStageFlagBits::kAll),
        renderable_headers.data(),
        renderable_headers.size() * sizeof(RenderableHeader)
    );

    // A pass to upload / rebuild TLAS
    {
        std::vector<int> visible_rt_static_mesh_renderable_indices;
        for (auto e : visible_renderable_indices) {
            if (auto static_mesh_inst = all_renderables[e]->As<StaticMeshInstance>()) {
                if (static_mesh_inst->GetStaticMesh()->IsRayTraced())
                    visible_rt_static_mesh_renderable_indices.push_back(e);
            }
        }
        auto instance_count = (uint32_t)visible_rt_static_mesh_renderable_indices.size();
        auto instance_data = builder.Allocate<RHIAccelerationStructureInstanceDesc>(instance_count);
        auto instance_data_bytesize = instance_count * sizeof(RHIAccelerationStructureInstanceDesc);
        for (const auto& [i, e] : std::views::enumerate(visible_rt_static_mesh_renderable_indices)) {
            auto renderable = all_renderables[e]->As<StaticMeshInstance>();
            auto data = RHIAccelerationStructureInstanceDesc {};
            data.instance_custom_index = e;
            data.mask = 0xFF; // Visible to all rays
            data.flags = RHIASGeometryInstanceFlagBits::kNone;
            data.acceleration_structure_reference = renderable->GetStaticMesh()->GetDeviceStaticMesh()->GetBLAS()->GetDeviceAddress();
            // Row major
            auto to_world_matrix = renderable->GetTransform().GetToWorldTransformMatrix();
            for (int x = 0; x < 4; x++)
                for (int y = 0; y < 3; y++)
                    data.transform[y * 4 + x] = to_world_matrix[x][y];
            instance_data[i] = data;
        }
        auto instance_buffer = builder.CreateBuffer(
            RHIBufferUsageFlagBits::kAccelerationStructureBuildInput,
            instance_data_bytesize
        );
        view->upload_context_.Add(instance_buffer.Raw(), instance_data, instance_data_bytesize);
        if (!view->scene_->GetDeviceScene()->TLAS_) {
            // Create one if not exists
            view->scene_->GetDeviceScene()->TLAS_ = RHI::Get().CreateAccelerationStructure(
                RHIAccelerationStructureType::kTopLevel
            );
        }
        auto TLAS = view->scene_->GetDeviceScene()->TLAS_;
        auto build_info = RHIAccelerationStructureBuildGeometryInfo {
            RHIAccelerationStructureType::kTopLevel,
            RHIAccelerationStructureBuildFlagBits::kPreferFastTrace
            | RHIAccelerationStructureBuildFlagBits::kAllowUpdate,
            RHIAccelerationStructureBuildMode::kUpdate,
            TLAS.Raw(), TLAS.Raw(),
            {},{},
            instance_count
        };
        auto build_sizes = TLAS->GetBuildSizes(build_info);
        bool rebuild = false;
        if (build_sizes.acceleration_structure_size > TLAS->GetSize()) {
            // Resize the TLAS if needed
            TLAS->Create(build_sizes.acceleration_structure_size);
            rebuild = true;
        }
        auto scratch_buffer = builder.CreateBuffer(
            RHIBufferUsageFlagBits::kAccelerationStructureScratch,
            rebuild ? build_sizes.build_scratch_size : build_sizes.update_scratch_size
        );
        builder.AddPass("Update TLAS", RDGPassFlagBits::kNeverCull,
            [instance_buffer = instance_buffer.Raw(), rebuild, build_info, scratch = scratch_buffer.Raw()]
            ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            auto as_build_info = build_info;
            as_build_info.instance_data = instance_buffer->GetRHI();
            as_build_info.mode = rebuild ? RHIAccelerationStructureBuildMode::kBuild : RHIAccelerationStructureBuildMode::kUpdate;
            queue.BuildAccelerationStructure(as_build_info, scratch->GetRHI());
        })->AddAS(TLAS.Raw(), RHIGPUAccessFlagBits::kAccelerationStructureWrite, RHIPipelineStageFlagBits::kAccelerationStructureBuild)
        ->AddBuffer(instance_buffer.Raw(), RHIGPUAccessFlagBits::kShaderRead)
        ->AddBuffer(scratch_buffer.Raw(), RHIGPUAccessFlagBits::kShaderRW);
    }

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
    builder.AddPass("ClearDepth", {},
        [depth = view->G_depth_.Raw()]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
        queue.ClearTexture(depth->GetRHI(), {1, 1, 1, 1});
    })->AddTexture(view->G_depth_.Raw(), RDGTextureUsageType::kTransferWrite);

    // Static meshes
    Render_DrawStaticMeshes(view, builder);

    // Volume primitives
    Render_DrawVolumePrimitives(view, builder);

    // Draw G-Buffer to output directly for debug purposes
    Render_DrawToOutput(view, builder, view->G_albedo_.Raw());

    // Update persistent data using current frame for next frame use
    view->UpdatePersistentData();

}

MI_NAMESPACE_END