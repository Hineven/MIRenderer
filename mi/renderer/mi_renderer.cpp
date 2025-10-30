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

#include "rdg/rdg_helper.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_noise.h"
#include "renderer/mi_volume_primitives.h"
#include "renderer/r_denoiser.h"
#include "renderer/r_diffuse_indirect_lighting.h"
#include "renderer/r_internal_common.h"
#include "renderer/r_light_structure.h"
#include "renderer/r_persistent.h"
#include "renderer/r_volume_primitives.h"
#include "renderer/r_world_radiance_cache.h"

MI_NAMESPACE_BEGIN
static CVar<int> CVar_FinalOutputType(
    "r.debug.final_output_type",
    "Final output on screen.\n"
    "0 - Radiance\n"
    "1 - Albedo\n"
    "2 - Direct lighting\n"
    "3 - Prev Radiance\n",
    0
);

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

void Renderer::FrameContext::Init() {

}


void Renderer::FrameContext::Deinit() {
    visible_renderables.clear();
    deferred_static_meshes = {};
    forward_static_meshes = {};
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
    view->SetupViewCommonShaderParameters(builder);
    // Allocate and set view->debug_common_params_
    view->SetupDebugCommonShaderParameters(builder);

    mi_assert(view->persistent_data_->view_index == 0, "Only one view is supported for now");
    auto all_renderables = view->scene_->GetRenderables();

    // Update dirty renderables with custom logic
    for (auto & e : all_renderables) {
        if (e && e->IsDirty()) {
            e->Update(view, builder);
        }
    }

    // Update scene AABB
    view->scene_->UpdateAABB();

    // Gather renderable common data for upload
    std::vector<glm::mat4x3> renderable_transforms;
    std::vector<glm::mat4x3> renderable_inverse_transforms;
    std::vector<glm::mat3x3> renderable_normal_transforms;
    std::vector<RenderableHeader> renderable_headers;
    std::vector<uint32_t> visible_renderable_indices;
    {
        renderable_transforms.reserve(all_renderables.size());
        renderable_inverse_transforms.reserve(all_renderables.size());
        renderable_headers.reserve(all_renderables.size());
        for (const auto& [i, e] : all_renderables | std::views::enumerate) {
            glm::mat4x3 to_world {};
            glm::mat4x3 to_local {};
            glm::mat3x3 normal_transform {};
            RenderableHeader renderable_header {};
            if (e) {
                to_world = e->GetTransform().GetToWorldTransformMatrix();
                to_local = e->GetTransform().GetToLocalTransformMatrix();
                normal_transform = glm::transpose(glm::inverse(glm::mat3(to_world)));
                renderable_header = e->GetDeviceRenderableHeader();
                // Clear dirty flag
                e->SetTransformDirty(false);
                if (e->IsVisible()) visible_renderable_indices.push_back(e->GetIndex());
            }
            renderable_transforms.push_back(to_world);
            renderable_inverse_transforms.push_back(to_local);
            renderable_normal_transforms.push_back(normal_transform);
            renderable_headers.push_back(renderable_header);
        }
    }
    // Upload renderable transforms and headers
    view->upload_context_.Add(
        builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw(),
            RHIGPUAccessFlagBits::kAll, RHIPipelineStageFlagBits::kAll),
        renderable_transforms.data(),
        renderable_transforms.size() * sizeof(glm::mat4x3));
    view->upload_context_.Add(
        builder.Import(view->scene_->GetDeviceScene()->d_renderable_inverse_transforms_.Raw(),
            RHIGPUAccessFlagBits::kAll, RHIPipelineStageFlagBits::kAll),
        renderable_inverse_transforms.data(),
        renderable_inverse_transforms.size() * sizeof(glm::mat4x3));
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

    // Prepare instance data for rebuilding TLAS
    std::vector<int> visible_rt_renderable_indices;
    for (auto e : visible_renderable_indices) {
        if (auto renderable = all_renderables[e]) {
            if (renderable->IsRayTraced()
                && renderable->IsVisible()
                && !renderable->IsEmpty()
                // Some ray-traced renderables have no ray-tracing enabled geometry
                // we have to check for that here.
                && renderable->GetBLAS()
                ) {
                visible_rt_renderable_indices.push_back(e);
            }
        }
    }
    auto instance_count = (uint32_t)visible_rt_renderable_indices.size();
    TRef<RDGBuffer> instance_buffer;
    if (instance_count > 0){
        auto instance_size = RHI::Get().GetAccelerationStructureInstanceStride();
        auto instance_data_bytesize = instance_count * instance_size;

        auto instance_data_raw = builder.Allocate<RHIAccelerationStructureInstanceDesc[]>(instance_count);

        auto instance_data = builder.Allocate(instance_data_bytesize);
        for (const auto& [i, e] : std::views::enumerate(visible_rt_renderable_indices)) {
            auto renderable = all_renderables[e];
            auto data = RHIAccelerationStructureInstanceDesc {};
            data.instance_custom_index = renderable->GetInstanceCustomIndex(); // 24 bits
            data.mask = 0xFF; // Visible to all rays
            // TODO support double sided & one sided geometries.
            data.flags = (uint32_t)RHIASGeometryInstanceFlagBits::kNone;
            data.acceleration_structure_reference = renderable->GetBLAS()->GetDeviceAddress();
            // Row major
            auto to_world_matrix = renderable->GetTransform().GetToWorldTransformMatrix();
            for (int x = 0; x < 4; x++)
                for (int y = 0; y < 3; y++)
                    data.transform[y * 4 + x] = to_world_matrix[x][y];
            instance_data_raw[i] = data;
        }
        // Convert to underlying device format
        RHI::Get().CreateAccelerationStructureInstances(instance_count, instance_data_raw, instance_data);
        instance_buffer = builder.CreateBuffer(
            RHIBufferUsageFlagBits::kAccelerationStructureBuildInput,
            instance_data_bytesize
        );
        view->upload_context_.Add(instance_buffer.Raw(), instance_data, instance_data_bytesize);
    }

    // Filter visible rendeables
    ctx.visible_renderables.reserve(all_renderables.size());
    for (auto & e : all_renderables) {
        if (e && e->IsVisible()) ctx.visible_renderables.push_back(e);
    }

    // Prepare static mesh draw commands
    Render_PrepareStaticMeshes(view, builder);

    // Fire batched uploads to the RDG
    view->upload_context_.Fire(builder);

    // Update TLAS
    {
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
        // If instance count changes, we must rebuild TLAS instead of update.
        const bool instance_count_changed = (view->scene_->GetDeviceScene()->tlas_instance_count_ != instance_count);
        // Query sizes with current (default Update) build_info first; we may re-query if we need a Build.
        auto build_sizes = TLAS->GetBuildSizes(build_info);
        bool rebuild = false;
        if (build_sizes.acceleration_structure_size > TLAS->GetSize()) {
            // Resize the TLAS if needed
            TLAS->Create(build_sizes.acceleration_structure_size);
            rebuild = true;
        }
        // Force rebuild if instance count changed (Update mode requires same primitive count)
        if (instance_count_changed) {
            rebuild = true;
        }
        // If we decided to rebuild, scratch size should use build_scratch_size; otherwise use update_scratch_size
        if (rebuild) {
            // Re-query sizes with Build mode to ensure scratch size is correct
            auto build_mode_info = build_info;
            build_mode_info.mode = RHIAccelerationStructureBuildMode::kBuild;
            build_sizes = TLAS->GetBuildSizes(build_mode_info);
        }
        auto scratch_buffer = builder.CreateBuffer(
            RHIBufferUsageFlagBits::kAccelerationStructureScratch,
            rebuild ? build_sizes.build_scratch_size : build_sizes.update_scratch_size
        );
        scratch_buffer->SetName("TLAS Update Scratch Buffer");
        builder.AddPass("Update TLAS", RDGPassFlagBits::kNeverCull,
            [instance_buffer = instance_buffer.Raw(), rebuild, build_info, scratch = scratch_buffer.Raw(), scene_ds = view->scene_->GetDeviceScene()]
            ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            // Barrier the previous update & use of the acceleration structure
            queue.AccelerationStructureBarrier(build_info.dst_acceleration_structure,
                RHIPipelineStageFlagBits::kAccelerationStructureBuild | RHIPipelineStageFlagBits::kRayTracing,
                RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIGPUAccessFlagBits::kAccelerationStructureRW,
                RHIGPUAccessFlagBits::kAccelerationStructureRW
            );
            auto as_build_info = build_info;
            as_build_info.instance_data = instance_buffer ? instance_buffer->GetRHI() : RHIBufferSpan{};
            as_build_info.mode = rebuild ? RHIAccelerationStructureBuildMode::kBuild : RHIAccelerationStructureBuildMode::kUpdate;
            queue.BuildAccelerationStructure(as_build_info, scratch->GetRHI());
            // Barrier the TLAS after building
            queue.AccelerationStructureBarrier(build_info.dst_acceleration_structure,
                RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIPipelineStageFlagBits::kRayTracing,
                RHIGPUAccessFlagBits::kAccelerationStructureRW,
                RHIGPUAccessFlagBits::kAccelerationStructureRead
            );
            // Record the instance count used for this TLAS build for future Update-vs-Build decisions
            scene_ds->tlas_instance_count_ = as_build_info.instance_count;
        })->AddASH_NoAutomaticBarrier(TLAS.Raw(), RHIGPUAccessFlagBits::kAccelerationStructureWrite, RHIPipelineStageFlagBits::kAccelerationStructureBuild) // AS barriers should be manually inserted
        ->AddBufferH(instance_buffer.Raw(), RHIGPUAccessFlagBits::kShaderRead, RHIPipelineStageFlagBits::kAccelerationStructureBuild)
        ->AddBufferH(scratch_buffer.Raw(), RHIGPUAccessFlagBits::kAccelerationStructureRW, RHIPipelineStageFlagBits::kAccelerationStructureBuild);
    }

    // Pre-allocate buffers that may be used among multiple lighting stages
    if (!view->volume_primitives_) {
        view->volume_primitives_ = new VolumePrimitivesViewData();
    }
    view->volume_primitives_->Allocate(builder, view);
    if (!view->world_cache_) {
        view->world_cache_ = new WorldRadianceCacheData();
    }
    view->world_cache_->Allocate(builder);
    if (!view->light_structure_) {
        view->light_structure_ = new LightStructureData();
    }
    view->light_structure_->Allocate(builder);
    if (!view->diffuse_indirect_lighting_data_) {
        view->diffuse_indirect_lighting_data_ = new DiffuseIndirectLightingData();
    }
    view->diffuse_indirect_lighting_data_->Allocate(builder, view);
    if (!view->denoiser_view_data_) {
        view->denoiser_view_data_ = new DenoiserViewData();
    }
    view->denoiser_view_data_->Allocate(builder, view);


    // Pre-allocate shared view persistent data among multiple lighting stages
    view->MakeSurePersistentDataExists(builder);

    // Ready for rendering

    // Try reset light structure history if needed
    Render_PrepareLightStructureHistory(view, builder);

    // Draw the sky first.
    Render_DrawSky(view, builder);

    // Clear G buffers
    builder.AddPass("ClearBuffers", {},
        [view]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
        queue.ClearTexture(view->G_normal_->GetRHI(), {});
        queue.ClearTexture(view->G_albedo_->GetRHI(), {});
        queue.ClearTexture(view->G_metallic_roughness_->GetRHI(), {});
        queue.ClearTexture(view->G_emission_->GetRHI(), {});
        queue.ClearTexture(view->G_flags_->GetRHI(), {});
        queue.ClearTexture(view->G_transmittance_->GetRHI(), {});
        queue.ClearTexture(view->shadow_map_moments_->GetRHI(), {});
    })
    ->AddTextureH(view->G_normal_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->G_albedo_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->G_metallic_roughness_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->G_emission_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->G_flags_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->G_transmittance_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->shadow_map_moments_.Raw(), RDGTextureUsageType::kTransferWrite);

	// Shadow map
    Render_DrawShadowMap(view, builder);

    // Static meshes
    Render_DrawDeferredStaticMeshes(view, builder);

    // Volume primitives
    Render_DrawVolumePrimitives(view, builder);

    // HiZ
    Render_ComputeHiZBuffer(view, builder);

    // Diffuse direct
    Render_ComputeDiffuseDirectLighting(view, builder);

    // Diffuse Indirect
    Render_ComputeDiffuseIndirectLighting(view, builder);

    // Stage light structure history
    Render_UpdateLightStructureHistory(view, builder);

    // Denoising
    Render_DenoiseLighting(view, builder);

    // Final composition
    Render_LightingComposition(view, builder);

    Render_DebugView(view, builder);

    auto type = CVar_FinalOutputType.Get();
    if (type == 0)
        Render_DrawToOutput(view, builder, view->radiance_.Raw());
    else if (type == 1)
        Render_DrawToOutput(view, builder, view->G_albedo_.Raw());
    else if (type == 2)
        Render_DrawToOutput(view, builder, view->G_depth_.Raw());
    else if (type == 3)
        Render_DrawToOutput(view, builder, view->G_normal_.Raw());
    else if (type == 4)
        Render_DrawToOutput(view, builder, view->G_transmittance_.Raw());
    else if (type == 5)
        Render_DrawToOutput(view, builder, view->diffuse_direct_lighting_.Raw());
    else if (type == 6)
        Render_DrawToOutput(view, builder, view->volume_direct_lighting_.Raw());
    else if (type == 7)
        Render_DrawToOutput(view, builder, view->diffuse_indirect_lighting_.Raw());
    else if (type == 8) {
        Render_PathTracing(view, builder);
        Render_DrawToOutput(view, builder, view->persistent_data_->path_tracing_film_.Raw());
    } else Render_DrawToOutput(view, builder, view->debug_output_.Raw());

    // Extra pass for forward rendering
    Render_DrawForwardStaticMeshes(view, builder);

    // Update persistent data using current frame for next frame use
    view->persistent_data_->FinalUpdate(view);

}

MI_NAMESPACE_END