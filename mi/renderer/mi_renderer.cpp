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
#include <rhi/rhi_ptlas.h>
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
#include "rdg/rdg_ray_tracing_registry.h"
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

#include "dlss/ngx_context.h"
#include "dlss/dlss_rr_context.h"

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
    light_structure = {};
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

TRef<RendererExports> Renderer::Render(RendererView * view, RenderGraphBuilder & builder) {
    TRef<RendererExports> exports(new RendererExports());
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
        return exports;
    }
    if (!view->scene_->GetDeviceScene()) {
        MI_WARN("Device scene is not present in the world.");
        return exports;
    }

    view->InitFrame();
    exports->RegisterResource("radiance", view->radiance_.Raw());
    exports->RegisterResource("overlay", view->overlay_.Raw());

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

    // If the scene contains any VolumePrimitives / VolumeGrid, enable related rendering.
    // Defaults to false and is flipped on below when a volume renderable is found.
    // When false, volume_*_lighting_ stay null and downstream passes (denoiser/composition)
    // bind them as nullptr -> treated as pure-black. No unwritten-texture flicker.
    bool should_render_volume_lighting = false;

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
                if (e->IsVisible()) {
                    visible_renderable_indices.push_back(e->GetIndex());
                    if (e->GetType() == RenderableType::kVolumeGridInstance || e->GetType() == RenderableType::kVolumePrimitivesInstance) {
                        should_render_volume_lighting = true;
                    }
                }
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

    // Prepare instance data for rebuilding TLAS.
    // Collect a flat list of BLAS instances across all visible ray-traced
    // renderables. A renderable contributes its GetGlobalBLASInstances (legacy
    // single-BLAS path, e.g. StaticMesh) + GetPartitionedBLASInstances (per-
    // chunk path, e.g. GigaVoxel). The TLAS is built over this flat list.
    struct GatheredInstance {
        Renderable * renderable;           // for transform-dirty / blas-updated flags
        RHIAccelerationStructure * blas;    // non-null
        const RenderableBLASInstance * desc;// points into the renderable's span
    };
    std::vector<GatheredInstance> gathered_instances;
    bool visible_rt_renderable_transform_dirty = false;
    for (auto e : visible_renderable_indices) {
        if (auto renderable = all_renderables[e]) {
            if (!renderable->IsRayTraced() || !renderable->IsVisible() || renderable->IsEmpty())
                continue;
            if (renderable->ClearTransformDirty()) {
                visible_rt_renderable_transform_dirty = true;
            }
            // New multi-instance path: global + partitioned.
            auto global_instances = renderable->GetGlobalBLASInstances();
            auto partitioned_instances = renderable->GetPartitionedBLASInstances();
            for (const auto & inst : global_instances) {
                if (inst.blas) gathered_instances.push_back({renderable, inst.blas, &inst});
            }
            for (const auto & inst : partitioned_instances) {
                if (inst.blas) gathered_instances.push_back({renderable, inst.blas, &inst});
            }
            // Legacy fallback: ONLY when no multi-instance override exists.
            // (A migrated renderable keeps GetBLAS() for vrt_hash / BLAS-update
            // tracking, but must not be double-added here.)
            if (global_instances.empty() && partitioned_instances.empty()) {
                if (auto * legacy_blas = renderable->GetBLAS()) {
                    gathered_instances.push_back({renderable, legacy_blas, nullptr});
                }
            }
        }
    }
    for (const auto& e : all_renderables) {
        if (e) e->ClearTransformDirty();
    }

    // TODO: batched update all BLAS (they are performed in renderable->Update currently)

    // If the set of BLAS handles changes, rebuild TLAS instead of update.
    // Hash over the flat BLAS handle list (order-sensitive).
    std::vector<void*> rt_handles;
    rt_handles.reserve(gathered_instances.size());
    bool vrt_blas_updated = false;
    for (const auto & gi : gathered_instances) {
        rt_handles.push_back(gi.blas->GetAPIHandle());
        vrt_blas_updated |= gi.renderable->IsBLASUpdated() || gi.renderable->IsTransformDirty();
        gi.renderable->SetBLASUpdated(false);
    }
    uint32_t vrt_hash = XXH32(rt_handles.data(), rt_handles.size() * sizeof(void*), 0);
    bool should_rebuild_tlas = vrt_hash != view->scene_->GetDeviceScene()->TLAS_vrt_hash_;
    bool should_update_tlas = visible_rt_renderable_transform_dirty || should_rebuild_tlas || vrt_blas_updated;

    // Filter visible rendeables
    ctx.visible_renderables.reserve(all_renderables.size());
    for (auto & e : all_renderables) {
        if (e && e->IsVisible()) ctx.visible_renderables.push_back(e);
    }

    // Prepare static mesh draw commands
    Render_PrepareStaticMeshes(view, builder);
    // Prepare GigaVoxel VC per-chunk draw commands
    Render_PrepareGigaVoxel(view, builder);
    // Prepare gaussian radiance fields (instance offsets/counts, renderable list, filter draw commands)
    Render_PrepareGaussianRadianceFields(view, builder);

    // Pre-allocate RDG resources that may be used among multiple lighting stages
    view->CreateSharedResources(builder, should_render_volume_lighting);
    // Pre-allocate shared view persistent data among multiple lighting stages
    view->MakeSurePersistentDataExists(builder);

    // Prepare light structure upload data (per-frame buffers for mesh light instances)
    Render_PrepareLightStructure(view, builder);

    // Fire batched uploads to the RDG
    view->upload_context_.Fire(builder);

    // Update vrt hash for scene (PTLAS rebuild decision uses this).
    if (should_rebuild_tlas) {
        view->scene_->GetDeviceScene()->TLAS_vrt_hash_ = vrt_hash;
    }

    // ---- Partitioned TLAS build (ALL ray-traced instances) ----
    // The renderer uses PTLAS exclusively for ray tracing. All gathered instances
    // (global, partitioned, legacy) are written into the PTLAS. Partitioned
    // instances (e.g. GigaVoxel per-chunk) use their assigned partition_index;
    // non-partitioned instances (StaticMesh etc.) use the GLOBAL partition.
    // Built via direct RHI queue access (bypassing RDG). Double-buffered.
    {
        auto scene_ds = view->scene_->GetDeviceScene();
        uint32_t instance_count = (uint32_t)gathered_instances.size();
        if (instance_count > 0 && should_update_tlas) {
            auto & queue = RHI::Get().GetGraphicsCommandQueue();
            auto props = RHI::Get().GetDeviceProperties();
            auto handle_size_aligned = RoundUp(
                props.shader_group_handle_size, props.shader_group_base_alignment);

            // Determine partition capacity + global instance count.
            uint32_t max_partition = 0;
            uint32_t global_instance_count = 0;
            for (const auto & gi : gathered_instances) {
                if (gi.desc) {
                    max_partition = std::max(max_partition, gi.desc->partition_index);
                } else {
                    global_instance_count++;
                }
            }
            uint32_t partition_count = max_partition + 1;

            // Capacity descriptor.
            RHIPartitionedTLASInstancesInput input {};
            input.flags = RHIAccelerationStructureBuildFlagBits::kPreferFastTrace
                        | RHIAccelerationStructureBuildFlagBits::kAllowUpdate;
            input.instance_count = instance_count;
            input.max_instance_per_partition_count = instance_count; // worst case
            input.partition_count = partition_count;
            input.max_instance_in_global_partition_count = global_instance_count;

            auto read_idx = scene_ds->ptlas_index_;
            auto write_idx = 1 - read_idx;

            // Create PTLAS resources if missing.
            if (!scene_ds->PTLAS_[0]) {
                scene_ds->PTLAS_[0] = RHI::Get().CreatePartitionedTLAS();
                scene_ds->PTLAS_[1] = RHI::Get().CreatePartitionedTLAS();
            }

            // (Re)allocate if capacity grew.
            bool need_rebuild = !scene_ds->ptlas_allocated_
                || scene_ds->ptlas_partition_count_ < partition_count
                || scene_ds->ptlas_instance_count_ < instance_count;
            if (need_rebuild) {
                auto sizes = scene_ds->PTLAS_[write_idx]->GetBuildSizes(input);
                scene_ds->PTLAS_[write_idx]->Allocate(sizes.acceleration_structure_size, input);
                scene_ds->PTLAS_[read_idx]->Allocate(sizes.acceleration_structure_size, input);
                scene_ds->ptlas_allocated_ = true;
                scene_ds->ptlas_partition_count_ = partition_count;
                scene_ds->ptlas_instance_count_ = instance_count;
            }

            // Prepare WRITE_INSTANCE data for ALL instances.
            std::vector<RHIPartitionedTLASWriteInstance> write_instances(instance_count);
            for (uint32_t i = 0; i < instance_count; i++) {
                const auto & gi = gathered_instances[i];
                auto renderable = gi.renderable;
                auto & wi = write_instances[i];
                wi.instance_index = i;

                if (gi.desc) {
                    // Multi-instance descriptor path (global or partitioned).
                    const auto & d = *gi.desc;
                    wi.instance_id = d.instance_custom_index;
                    wi.instance_mask = d.instance_mask;
                    wi.instance_contribution_to_hit_group_index =
                        d.instance_contribution_to_hit_group_index * handle_size_aligned;
                    wi.partition_index = d.partition_index;
                    wi.acceleration_structure = gi.blas->GetDeviceAddress();
                    // Transform (row-major 3x4).
                    auto to_world = d.transform.GetToWorldTransformMatrix();
                    for (int x = 0; x < 4; x++)
                        for (int y = 0; y < 3; y++)
                            wi.transform[y * 4 + x] = to_world[x][y];
                    // Explicit AABB if available (partitioned instances have it).
                    if (d.partition_index != kPTLASPartitionIndexGlobal && d.explicit_aabb.IsValid()) {
                        wi.explicit_aabb[0] = d.explicit_aabb.min.x;
                        wi.explicit_aabb[1] = d.explicit_aabb.min.y;
                        wi.explicit_aabb[2] = d.explicit_aabb.min.z;
                        wi.explicit_aabb[3] = d.explicit_aabb.max.x;
                        wi.explicit_aabb[4] = d.explicit_aabb.max.y;
                        wi.explicit_aabb[5] = d.explicit_aabb.max.z;
                        wi.instance_flags = RHIPTLASInstanceFlagBits::kEnableExplicitAABB
                                          | RHIPTLASInstanceFlagBits::kDisableTriangleCulling;
                    } else {
                        wi.instance_flags = RHIPTLASInstanceFlagBits::kDisableTriangleCulling;
                    }
                } else {
                    // Legacy single-BLAS path (global partition, no explicit AABB).
                    wi.instance_id = renderable->GetInstanceCustomIndex();
                    wi.instance_mask = 0xFF;
                    uint32_t class_index = renderable->GetRayTracedClassIndex();
                    wi.instance_contribution_to_hit_group_index = class_index * handle_size_aligned;
                    wi.partition_index = kPTLASPartitionIndexGlobal;
                    wi.acceleration_structure = renderable->GetBLAS()->GetDeviceAddress();
                    auto to_world = renderable->GetTransform().GetToWorldTransformMatrix();
                    for (int x = 0; x < 4; x++)
                        for (int y = 0; y < 3; y++)
                            wi.transform[y * 4 + x] = to_world[x][y];
                    wi.instance_flags = RHIPTLASInstanceFlagBits::kDisableTriangleCulling;
                }
            }

            // Upload write_instances to a device buffer.
            size_t write_data_size = instance_count * sizeof(RHIPartitionedTLASWriteInstance);
            auto write_data_buffer = RHI::Get().CreateBuffer(
                write_data_size, RHIBufferUsageFlagBits::kAccelerationStructureBuildInput);
            {
                auto staging = RHI::Get().CreateBuffer(write_data_size, RHIBufferUsageFlagBits::kStaging);
                memcpy(staging->Map(), write_instances.data(), write_data_size);
                staging->Unmap();
                queue.CopyBuffer(staging->GetSpan(), write_data_buffer->GetSpan());
            }

            // Indirect commands buffer + count buffer (device-addressable, for vkCmdUpdateBuffer).
            auto indirect_cmds_buffer = RHI::Get().CreateBuffer(
                256, RHIBufferUsageFlagBits::kShaderDeviceAddress);
            auto indirect_count_buffer = RHI::Get().CreateBuffer(
                16, RHIBufferUsageFlagBits::kShaderDeviceAddress);

            // Scratch buffer.
            auto scratch_sizes = scene_ds->PTLAS_[write_idx]->GetBuildSizes(input);
            auto scratch_buffer = RHI::Get().CreateBuffer(
                scratch_sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);

            // Barrier: wait for upload before build.
            queue.BufferBarrier(write_data_buffer->GetSpan(),
                RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kShaderRead);

            // Build op: one WRITE_INSTANCE op covering all instances.
            RHIPartitionedTLASBuildOp op {};
            op.op_type = RHIPTLASOpType::kWriteInstance;
            op.arg_count = instance_count;
            op.arg_data = write_data_buffer->GetDeviceAddress();
            op.arg_stride = sizeof(RHIPartitionedTLASWriteInstance);

            // First build (src=nullptr) vs update (src=read side).
            RHIPartitionedTLAS * src_ptlas = need_rebuild ? nullptr : scene_ds->PTLAS_[read_idx].Raw();

            queue.BuildPartitionedTLAS(
                scene_ds->PTLAS_[write_idx].Raw(), src_ptlas,
                scratch_buffer->GetSpan(),
                std::span<const RHIPartitionedTLASBuildOp>(&op, 1),
                indirect_cmds_buffer->GetSpan(),
                indirect_count_buffer->GetSpan(),
                input);

            queue.WaitForIdle();

            // Swap: the newly-built write side becomes the read side.
            scene_ds->ptlas_index_ = write_idx;
        }
    }

    if (view->g_buffer_) {
        exports->RegisterResource("depth", view->g_buffer_->G_depth_.Raw());
        exports->RegisterResource("transmittance", view->g_buffer_->G_transmittance_.Raw());
        exports->RegisterResource("visibility", view->g_buffer_->G_visibility_.Raw());
        exports->RegisterResource("albedo", view->g_buffer_->G_albedo_.Raw());
        exports->RegisterResource("normal", view->g_buffer_->G_normal_.Raw());
        exports->RegisterResource("geometry_normal", view->g_buffer_->G_geometry_normal_.Raw());
        exports->RegisterResource("motion_vector", view->g_buffer_->G_motion_vector_.Raw());
    }

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
        // G_transmittance uses multiplicative semantics: 1.0 = no extinction (light passes through),
        // 0.0 = fully blocked. The identity/default for any pixel without volume is 1.0.
        // The ONLY writer of this texture is DrawVolumePrimitives (which assigns absolute
        // transmittance, never accumulates), and that pass is skipped when the scene has no volume
        // data. Clearing to 0 (the previous default) left every pixel at transmittance 0 in that
        // case, so LightingComposition computed Radiance = SurfaceRadiance * 0 = 0 -> black screen.
        // Clearing to 1.0 is correct in both modes: overwritten per-pixel when volume is on,
        // and supplies the correct identity transmittance when volume is off.
        queue.ClearTexture(view->g_buffer_->G_transmittance_->GetRHI(), {1.f, 1.f, 1.f, 1.f});
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
    // GigaVoxel VC chunks (layers on top of static-mesh visibility/depth, kLoad).
    Render_DrawGigaVoxelVC(view, builder);
    // Unified visibility decode -> G-buffer (resolves static mesh + GigaVoxel).
    Render_DecodeVisibility(view, builder);

    // Volume primitives
    if (should_render_volume_lighting) {
        Render_DrawVolumePrimitives(view, builder);
    }
    // When should_render_volume_lighting is false, volume_primitives_ is null.
    // Downstream shaders read null binding as zero → "no volume".
    if (view->volume_primitives_) {
        exports->RegisterResource("volume_sample_color", view->volume_primitives_->volume_sample_color_.Raw());
        exports->RegisterResource("volume_sample_linear_depth", view->volume_primitives_->volume_sample_linear_depth_.Raw());
        exports->RegisterResource("volume_density", view->volume_primitives_->G_volume_density_.Raw());
        exports->RegisterResource("volume_color", view->volume_primitives_->G_volume_color_.Raw());
    }

    // HiZ
    Render_ComputeHiZBuffer(view, builder);

    // Diffuse direct
    Render_ComputeDiffuseDirectLighting(view, builder);

    // Volume direct (must run before denoiser prefilter which reads VolumeDirectLightingTexture)
    if (should_render_volume_lighting)
        Render_ComputeVolumeDirectLighting(view, builder);

    // Volume grid direct
    if (should_render_volume_lighting)
        Render_ComputeVolumeGridDirectLighting(view, builder);

    {
        RDGSectionGuard section(builder, "IndirectLighting");

        // Initialize & reuse the hash grid cache from the previous frame before updating.
        Render_PrepareHashGridCache(view, builder);

        // Indirect lighting
        Render_UpdateDiffuseIndirectLighting(view, builder);
        if (should_render_volume_lighting)
            Render_UpdateVolumeIndirectLighting(view, builder);

        Render_UpdateHashGridCache(view, builder);

        Render_FinishDiffuseIndirectLighting(view, builder);
        if (should_render_volume_lighting)
            Render_FinishVolumeIndirectLighting(view, builder);
    }

    // Stage light structure history
    Render_UpdateLightStructureHistory(view, builder);

    // Denoising
    Render_DenoiseLighting(view, builder);
    if (view->diffuse_direct_lighting_) {
        exports->RegisterResource("diffuse_direct", view->diffuse_direct_lighting_->radiance.Raw());
    }
    if (view->diffuse_indirect_lighting_) {
        exports->RegisterResource("diffuse_indirect", view->diffuse_indirect_lighting_->radiance.Raw());
    }
    if (view->volume_direct_lighting_) {
        exports->RegisterResource("volume_direct", view->volume_direct_lighting_->radiance.Raw());
    }
    if (view->volume_indirect_lighting_) {
        exports->RegisterResource("volume_indirect", view->volume_indirect_lighting_->radiance.Raw());
    }
    if (view->denoiser_) {
        exports->RegisterResource("denoised_diffuse_direct", view->denoiser_->denoised_diffuse_direct_lighting.Raw());
        exports->RegisterResource("denoised_diffuse_indirect", view->denoiser_->denoised_diffuse_indirect_lighting.Raw());
    }

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
        Render_DrawToOutput(view, builder, view->volume_direct_lighting_ ? view->volume_direct_lighting_->radiance.Raw() : nullptr);
    else if (type == 7)
        Render_DrawToOutput(view, builder, view->diffuse_indirect_lighting_->radiance.Raw());
    else if (type == 8) {
        Render_PathTracing(view, builder);
        Render_DrawToOutput(view, builder, view->debug_output_.Raw());
    } else if (type == 9) {
        Render_DrawToOutput(view, builder, view->diffuse_indirect_lighting_->screen_probe_radiance_depth.Raw());
    } else Render_DrawToOutput(view, builder, view->debug_output_.Raw());

    // Clear overlay
    Helpers::Clear(builder, view->overlay_.Raw(), glm::vec4(0,0,0,0));

    // Draw gaussian radiance fields directly to overlay (color does not participate in lighting composition)
    Render_DrawGaussianRadianceFields(view, builder);

    // Extra pass for forward rendering (drawn to overlay)
    Render_DrawForwardStaticMeshes(view, builder);

    // Composite overlay to backbuffer (sRGB conversion)
    Render_DrawToOutput(view, builder, view->overlay_.Raw(), DrawToOutputMappingType::eLinearToSRGB);

    if (view->grf_) {
        exports->RegisterResource("grf_depth", view->grf_->stochastic_rendering_depth_.Raw());
        exports->RegisterResource("grf_opacity", view->grf_->stochastic_rendering_opacity_.Raw());
    }
    if (view->persistent_data_ && view->persistent_data_->path_tracing_film_) {
        exports->RegisterResource("path_tracing_film", view->persistent_data_->path_tracing_film_.Raw());
    }

    // Update persistent data using current frame for next frame use
    view->persistent_data_->FinalUpdate(view);

    return exports;
}

MI_NAMESPACE_END
