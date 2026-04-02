/*
 * Created: 2025/4/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <algorithm>
#include <ranges>

#include "shaders/shared/SharedRenderable.hlsl"

#include <barrier>
#include <xxhash.h>
#include <core/infra.h>
#include <rhi/rhi_as.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_cmd.h>

#include <renderer/mi_renderer.h>
#include <renderer/mi_static_mesh.h>
#include <renderer/mi_renderer_view.h>
#include <renderer/mi_material.h>
#include <renderer/mi_gaussian_radiance_field.h>

#include "core/util/debug_prof.h"
#include "core/util/unordered_hashing.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_noise.h"
#include "renderer/mi_volume_primitives.h"
#include "renderer/r_denoiser.h"
#include "renderer/r_diffuse_direct_lighting.h"
#include "renderer/r_diffuse_indirect_lighting.h"
#include "include/renderer/r_geometry_buffer.h"
#include "renderer/r_gaussian_radiance_field.h"
#include "renderer/r_internal_common.h"
#include "renderer/r_light_structure.h"
#include "renderer/r_persistent.h"
#include "renderer/r_volume_direct_lighting.h"
#include "renderer/r_volume_indirect_lighting.h"
#include "renderer/r_volume_grid_direct_lighting.h"
#include "renderer/r_volume_primitives.h"
#include "renderer/r_world_radiance_cache.h"

MI_NAMESPACE_BEGIN
static CVar<int> CVar_FinalOutputType(
    "r.debug.final_output_type",
    "Final output on screen.\n"
    "0 - Radiance\n"
    "1 - Albedo\n"
    "...\n",
    0
);

static CVar<bool> CVar_EnableTAA(
    "r.postprocessing.enable_taa",
    "Enable temporal anti-aliasing before tonemapping.",
    true
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

// A: An unordered hash of renderable TLAS handles & TLAS configurations. (Rebuild required)
// B: BLAS updated / rebuilt? (Update required)
// This is used to determine if we should update or rebuild the TLAS.
static std::pair<uint32_t, bool> ComputeRenderableStructureHashAndClearBLASUpdatedFlags (
    const std::vector<uint32_t> & visible_rt_renderables, const std::vector<Renderable*> & all_renderables) {
    std::vector<void*> handles;
    handles.reserve(visible_rt_renderables.size());
    bool h2 {};
    for (auto index : visible_rt_renderables) {
        auto e = all_renderables[index];
        auto e1 = e->GetBLAS()->GetAPIHandle();
        handles.push_back(e1);
        h2 |= e->IsBLASUpdated() || e->IsTransformDirty();
        e->SetBLASUpdated(false);
    }
    // The order of the handles is crucial here, as different order means different TLAS structure. (Though they are
    // unordered in the represented TLAS scene.)
    auto h1 = XXH32(handles.data(), handles.size() * sizeof(void*), 0);
    return {h1, h2};
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

    // Stage history buffers for motion vectors (GPU-side copy before the current frame)
    {
        auto curr_transform = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
        auto prev_transform = builder.Import(device_allocator_->GetPrevRenderableTransformBuffer());
        auto curr_hash = builder.Import(device_allocator_->GetRenderableHashBuffer());
        auto prev_hash = builder.Import(device_allocator_->GetPrevRenderableHashBuffer());
        builder.AddPass("StageRenderableHistory", RDGPassFlagBits::kNeverCull,
            [curr_transform, prev_transform, curr_hash, prev_hash]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
                queue.CopyBuffer(curr_transform->GetRHI(), prev_transform->GetRHI());
                queue.CopyBuffer(curr_hash->GetRHI(), prev_hash->GetRHI());
            })
            ->AddBufferH(prev_transform, RHIGPUAccessFlagBits::kTransferWrite)
            ->AddBufferH(curr_transform, RHIGPUAccessFlagBits::kTransferRead)
            ->AddBufferH(prev_hash, RHIGPUAccessFlagBits::kTransferWrite)
            ->AddBufferH(curr_hash, RHIGPUAccessFlagBits::kTransferRead);
    }

    // Allocate and set view->view_common_params_
    view->SetupViewCommonShaderParameters(builder);
    // Allocate and set view->debug_common_params_
    view->SetupDebugCommonShaderParameters(builder);

    mi_assert(view->persistent_data_->view_index_ == 0, "Only one view is supported for now");
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
    std::vector<uint32_t> renderable_hashes;
    {
        renderable_transforms.reserve(all_renderables.size());
        renderable_inverse_transforms.reserve(all_renderables.size());
        renderable_headers.reserve(all_renderables.size());
        renderable_hashes.reserve(all_renderables.size());
        for (const auto& [i, e] : all_renderables | std::views::enumerate) {
            glm::mat4x3 to_world {};
            glm::mat4x3 to_local {};
            glm::mat3x3 normal_transform {};
            RenderableHeader renderable_header {};
            uint32_t renderable_hash = 0;
            if (e) {
                to_world = e->GetTransform().GetToWorldTransformMatrix();
                to_local = e->GetTransform().GetToLocalTransformMatrix();
                normal_transform = glm::transpose(glm::inverse(glm::mat3(to_world)));
                renderable_header = e->GetDeviceRenderableHeader();
                renderable_hash = e->GetHash();
                if (e->IsVisible()) visible_renderable_indices.push_back(e->GetIndex());
            }
            renderable_transforms.push_back(to_world);
            renderable_inverse_transforms.push_back(to_local);
            renderable_normal_transforms.push_back(normal_transform);
            renderable_headers.push_back(renderable_header);
            renderable_hashes.push_back(renderable_hash);
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
    // Upload renderable hashes
    view->upload_context_.Add(
        builder.Import(device_allocator_->GetRenderableHashBuffer(),
            RHIGPUAccessFlagBits::kAll, RHIPipelineStageFlagBits::kAll),
        renderable_hashes.data(),
        renderable_hashes.size() * sizeof(uint32_t)
    );

    // Prepare instance data for rebuilding TLAS
    std::vector<uint32_t> visible_rt_renderable_indices;
    bool visible_rt_renderable_transform_dirty = false;
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
                if (renderable->ClearTransformDirty()) {
                    visible_rt_renderable_transform_dirty = true;
                }
            }
        }
    }
    for (const auto& e : all_renderables) {
        if (e) e->ClearTransformDirty();
    }

    // TODO: batched update all BLAS (they are performed in renderable->Update currently)

    // If instance count changes, we must rebuild TLAS instead of update.
    auto [vrt_hash, vrt_blas_updated] = ComputeRenderableStructureHashAndClearBLASUpdatedFlags(visible_rt_renderable_indices, all_renderables);
    bool should_rebuild_tlas = vrt_hash != view->scene_->GetDeviceScene()->TLAS_vrt_hash_;
    bool should_update_tlas = visible_rt_renderable_transform_dirty || should_rebuild_tlas || vrt_blas_updated;
    TRef<RDGBuffer> tlas_instance_buffer;
    if (should_update_tlas) {
        auto instance_count = (uint32_t)visible_rt_renderable_indices.size();
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
                // Disable back face culling for renderables with double-sided materials.
                data.flags = renderable->GetASGeometryInstanceFlags();
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
            tlas_instance_buffer = builder.CreateBuffer(
                RHIBufferUsageFlagBits::kAccelerationStructureBuildInput,
                instance_data_bytesize
            );
            view->upload_context_.Add(tlas_instance_buffer.Raw(), instance_data, instance_data_bytesize);
        }
    }

    // Filter visible rendeables
    ctx.visible_renderables.reserve(all_renderables.size());
    for (auto & e : all_renderables) {
        if (e && e->IsVisible()) ctx.visible_renderables.push_back(e);
    }

    // Prepare static mesh draw commands
    Render_PrepareStaticMeshes(view, builder);
    // Prepare gaussian radiance fields (instance offsets/counts, renderable list, filter draw commands)
    Render_PrepareGaussianRadianceFields(view, builder);

    // Fire batched uploads to the RDG
    view->upload_context_.Fire(builder);

    // Update TLAS
    if (should_update_tlas) {
        auto instance_count = (uint32_t)visible_rt_renderable_indices.size();
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
        // Query sizes with current (default Update) build_info first; we may re-query if we need a Build.
        auto build_sizes = TLAS->GetBuildSizes(build_info);
        // Condition 1: Requested a larger TLAS allocation
        if (build_sizes.acceleration_structure_size > TLAS->GetSize()) {
            // Resize the TLAS if needed
            TLAS->Create(build_sizes.acceleration_structure_size);
            should_rebuild_tlas = true;
        }

        // If we decided to rebuild, scratch size should use build_scratch_size; otherwise use update_scratch_size
        if (should_rebuild_tlas) {
            // Re-query sizes with Build mode to ensure scratch size is correct
            auto build_mode_info = build_info;
            build_mode_info.mode = RHIAccelerationStructureBuildMode::kBuild;
            build_sizes = TLAS->GetBuildSizes(build_mode_info);
            // Update vrt hash for scene
            view->scene_->GetDeviceScene()->TLAS_vrt_hash_ = vrt_hash;
        }
        auto scratch_buffer = builder.CreateBuffer(
            RHIBufferUsageFlagBits::kAccelerationStructureScratch,
            should_rebuild_tlas ? build_sizes.build_scratch_size : build_sizes.update_scratch_size
        );
        scratch_buffer->SetName("TLAS Update Scratch Buffer");
        builder.AddPass("Update TLAS", RDGPassFlagBits::kNeverCull,
            [tlas_instance_buffer = tlas_instance_buffer.Raw(), should_rebuild_tlas, build_info, scratch = scratch_buffer.Raw(), scene_ds = view->scene_->GetDeviceScene()]
            ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            // Barrier the previous update & use of the acceleration structure
            queue.AccelerationStructureBarrier(build_info.dst_acceleration_structure,
                RHIPipelineStageFlagBits::kAccelerationStructureBuild | RHIPipelineStageFlagBits::kRayTracing,
                RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIGPUAccessFlagBits::kAccelerationStructureRW,
                RHIGPUAccessFlagBits::kAccelerationStructureRW
            );
            auto as_build_info = build_info;
            as_build_info.instance_data = tlas_instance_buffer ? tlas_instance_buffer->GetRHI() : RHIBufferSpan{};
            as_build_info.mode = should_rebuild_tlas ? RHIAccelerationStructureBuildMode::kBuild : RHIAccelerationStructureBuildMode::kUpdate;
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
        ->AddBufferH(tlas_instance_buffer.Raw(), RHIGPUAccessFlagBits::kShaderRead, RHIPipelineStageFlagBits::kAccelerationStructureBuild)
        ->AddBufferH(scratch_buffer.Raw(), RHIGPUAccessFlagBits::kAccelerationStructureRW, RHIPipelineStageFlagBits::kAccelerationStructureBuild);
    }

    // Pre-allocate RDG resources that may be used among multiple lighting stages
    view->CreateSharedResources(builder);

    // Pre-allocate shared view persistent data among multiple lighting stages
    view->MakeSurePersistentDataExists(builder);

    // Ready for rendering

    // Try reset light structure history if needed
    Render_PrepareLightStructureHistory(view, builder);

    // Build light structure for light sampling.
    Render_BuildLightStructure(view, builder);

    // Draw the sky first.
    Render_DrawSky(view, builder);

    // Clear G buffers
    builder.AddPass("ClearBuffers", {},
        [view]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
        queue.ClearTexture(view->g_buffer_->G_normal_->GetRHI(), {});
        queue.ClearTexture(view->g_buffer_->G_albedo_->GetRHI(), {});
        queue.ClearTexture(view->g_buffer_->G_metallic_roughness_->GetRHI(), {});
        queue.ClearTexture(view->g_buffer_->G_emission_->GetRHI(), {});
        queue.ClearTexture(view->g_buffer_->G_flags_->GetRHI(), {});
        queue.ClearTexture(view->g_buffer_->G_transmittance_->GetRHI(), {});
        queue.ClearTexture(view->shadow_map_moments_->GetRHI(), {});
    })
    ->AddTextureH(view->g_buffer_->G_normal_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->g_buffer_->G_albedo_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->g_buffer_->G_metallic_roughness_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->g_buffer_->G_emission_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->g_buffer_->G_flags_.Raw(), RDGTextureUsageType::kTransferWrite)
    ->AddTextureH(view->g_buffer_->G_transmittance_.Raw(), RDGTextureUsageType::kTransferWrite)
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

    // Volume direct (must run before denoiser prefilter which reads VolumeDirectLightingTexture)
    Render_ComputeVolumeDirectLighting(view, builder);

    // Volume grid direct
    Render_ComputeVolumeGridDirectLighting(view, builder);

    {
        RDGSectionGuard section(builder, "IndirectLighting");

        // Initialize & reuse the hash grid cache from the previous frame before updating.
        Render_PrepareHashGridCache(view, builder);

        // Indirect lighting
        Render_UpdateDiffuseIndirectLighting(view, builder);
        Render_UpdateVolumeIndirectLighting(view, builder);

        Render_UpdateHashGridCache(view, builder);

        Render_FinishDiffuseIndirectLighting(view, builder);
        Render_FinishVolumeIndirectLighting(view, builder);
    }

    // Stage light structure history
    Render_UpdateLightStructureHistory(view, builder);

    // Denoising
    Render_DenoiseLighting(view, builder);

    // Final composition
    Render_LightingComposition(view, builder);

    auto type = CVar_FinalOutputType.Get();
    if (type == 0) {
        auto flags = CVar_EnableTAA.Get() ? PostProcessingFlagBits::eEnableTAA : PostProcessingFlagBits::eNone;
        Render_DrawToOutput(view, builder, view->radiance_.Raw(), DrawToOutputMappingType::eRadianceToSRGB, flags);
    }
    else if (type == 1)
        Render_DrawToOutput(view, builder, view->g_buffer_->G_albedo_.Raw());
    else if (type == 2)
        Render_DrawToOutput(view, builder, view->g_buffer_->G_depth_.Raw());
    else if (type == 3)
        Render_DrawToOutput(view, builder, view->g_buffer_->G_normal_.Raw());
    else if (type == 4)
        Render_DrawToOutput(view, builder, view->g_buffer_->G_motion_vector_.Raw());
    else if (type == 5)
        Render_DrawToOutput(view, builder, view->diffuse_direct_lighting_->radiance.Raw());
    else if (type == 6)
        Render_DrawToOutput(view, builder, view->volume_direct_lighting_->radiance.Raw());
    else if (type == 7)
        Render_DrawToOutput(view, builder, view->diffuse_indirect_lighting_->radiance.Raw());
    else if (type == 8) {
        Render_PathTracing(view, builder);
        Render_DrawToOutput(view, builder, view->persistent_data_->path_tracing_film_.Raw());
    } else Render_DrawToOutput(view, builder, view->debug_output_.Raw());

    // Clear overlay
    Helpers::Clear(builder, view->overlay_.Raw(), glm::vec4(0,0,0,0));

    // Draw gaussian radiance fields directly to overlay (color does not participate in lighting composition)
    Render_DrawGaussianRadianceFields(view, builder);

    // Extra pass for forward rendering (drawn to overlay)
    Render_DrawForwardStaticMeshes(view, builder);

    // Composite overlay to backbuffer (sRGB conversion)
    Render_DrawToOutput(view, builder, view->overlay_.Raw(), DrawToOutputMappingType::eLinearToSRGB);

    // Update persistent data using current frame for next frame use
    view->persistent_data_->FinalUpdate(view);

}

MI_NAMESPACE_END
