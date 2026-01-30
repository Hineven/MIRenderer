/*
 * Created: 2025/9/19
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
#include "renderer/mi_volume_primitives.h"
#include "renderer/r_geometry_buffer.h"
#include "r_view_common.h"
#include "r_volume_primitives.h"
#include "r_world_radiance_cache.h"
#include "r_debug.h"
#include "../shaders/shared/SharedLight.hlsl"
MI_NAMESPACE_BEGIN

static CVar<int> CVar_DebugViewMode(
    "r.debug_view.mode",
    "Debug view mode. 0: visualize raytracing scene, 1: visualize traced rays",
    0
);

static CVar<int> CVar_DebugViewVisualizeRayColors(
    "r.debug_view.visualize_ray_colors",
    "0: disabled, 1: enabled",
    0
);

DebugPersistentData::DebugPersistentData() {

}

DebugPersistentData::~DebugPersistentData() {

}

void DebugPersistentData::MakeSureExists([[maybe_unused]] RendererView * view, [[maybe_unused]] RenderGraphBuilder & builder) {
    // Do nothing.
}

class VisualizeRayTracingSceneShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(AccelerationStructure, TLAS)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumePrimitivesHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDebugOutput)
        SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(VisualizeRayTracingSceneShader, "mi/renderer/shaders/VisualizeRayTracingScene.hlsl",
                                "VisualizeRayTracingSceneRaygen",
                                 "VisualizeRayTracingSceneClosestHit", "VisualizeRayTracingSceneAnyHit", "VisualizeRayTracingSceneMiss")

class VisualizeTracedRaysShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, TracedRaysOriginBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, TracedRaysDirectionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, TracedRaysStateBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, TracedRaysColorBuffer)

        SHADER_RENDER_TARGET(PixelFormatType::kR16G16B16A16_FLOAT, DebugOutput)
        SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderOptionalMacros () {
        return {"VISUALIZE_RAY_COLORS"};
    }

    static RDGShaderPipelineConfig GetShaderPipelineConfig () {
        RDGShaderPipelineConfig config {};
        // Enable comparison but disable depth write
        config.depth_test_enabled = true;
        config.depth_write_enabled = false;
        // Reversed-z
        config.depth_compare_op = RHIDepthCompareOpType::kGreater;
        // Render lines only
        config.topology = RHIPrimitiveTopologyType::kLineList;
        return config;
    }
};

IMPLEMENT_RDG_GRAPHICS_SHADER(VisualizeTracedRaysShader, "mi/renderer/shaders/VisualizeTracedRays.hlsl", "VisualizeTracedRaysVS", "VisualizeTracedRaysPS")

class VisualizeWorldCacheShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(Texture2D, PreviousShadedDiffuseRadianceWithoutEmission)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDebugOutputTexture)
        // Tile allocator
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_FreeTileCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_FreeTileListBuffer)
        // Hash table for tiles
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_BucketHashBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_BucketTileIndexBuffer) // Store indices of tiles in the bucket
        // Tile properties
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_TileTimestampBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_TileBucketHashBuffer)
        // Cell values (radiance) cached in hash grids
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_CellValueBuffer) // 2 elements per cell
        // This is quantilized and atomic accumulated, and only contains mip0 cells
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_UpdateCellValueXBuffer) // 4 elements per cell
        // A list of tiles that should be updated this frame
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_UpdateTileCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_UpdateTileListBuffer)
        // A list of active tile indices
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileCountBeforeAllocationBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileListBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_HistoryActiveTileCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_HistoryActiveTileListBuffer)

        SHADER_UNIFORM_BUFFER(HashGridWorldCacheUB, HashGrids_UB)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER(VisualizeWorldCacheShader, "mi/renderer/shaders/VisualizeWorldCache.hlsl", "VisualizeWorldCache")

class VisualizeSpatialPositionsShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDebugOutputTexture)

        SHADER_RESOURCE_PARAMETER(StructuredBuffer, SpatialPositionsBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, SpatialPositionsCount)

        SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(VisualizeSpatialPositionsShader, "mi/renderer/shaders/VisualizeSpatialPositions.hlsl", "VisualizeSpatialPositions")

void Renderer::Render_DebugView(RendererView *view, RenderGraphBuilder &builder) {
    // Visualize the scene used for ray tracing
    {
        view->debug_views_.visualize_ray_tracing_scene_output_ = builder.CreateTexture2D(view->film_width_, view->film_height_,
            PixelFormatType::kR16G16B16A16_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource
            | RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer);

        auto shader = RDGShaderLibrary::Get().GetShader<VisualizeRayTracingSceneShader>();
        auto params = builder.Allocate<VisualizeRayTracingSceneShader::Params>();
        params->View = view->view_common_params_;
        params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
        params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
        params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
        params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
        params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
        params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
        params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
        params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
        params->VolumePrimitivesHeaderBuffer = builder.Import(device_allocator_->GetVolumePrimitivesHeaderBuffer());
        params->PrimitiveData = builder.Import(
            device_allocator_->GetCustomUberBuffer(VolumePrimitives::kVolumePrimitiveAllocatorUberBufferIndex)->GetRHI()
        );
        params->RWDebugOutput = view->debug_views_.visualize_ray_tracing_scene_output_.Raw();
        if (view->scene_->GetSkyTexture()) {
            params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
        } else {
            params->EnvironmentMap = nullptr;
        }
        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
        Helpers::AddTraceRaysPass(builder, shader, params, view->film_width_, view->film_height_);
    }
    // Visualize traced rays
    {
        if (!view->debug_buffers_.traced_ray_origins || !view->debug_buffers_.traced_ray_directions
            || !view->debug_buffers_.traced_ray_states || !view->debug_buffers_.traced_ray_count) {
            // Robustness feature makes sure that null -> 0 rays, so we dont need to do anything except popping up a warning
            // MI_WARN("No traced rays debug buffers, cannot visualize traced rays");
        }
        // Prepare debug output
        view->debug_views_.visualize_traced_rays_output_ = builder.CreateTexture2D(view->film_width_, view->film_height_,
            PixelFormatType::kR16G16B16A16_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource
            | RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer);
        // Duplicate lighting to debug output
        Helpers::CopyTexture(builder, view->radiance_.Raw(), view->debug_views_.visualize_traced_rays_output_.Raw());

        bool should_visualize_colors = CVar_DebugViewVisualizeRayColors.Get() && view->debug_buffers_.traced_ray_colors;
        auto ini = RDGShaderInitializationInfo {};
        if (should_visualize_colors) ini.optional_macros.push_back("VISUALIZE_RAY_COLORS");
        auto shader = RDGShaderLibrary::Get().GetShader<VisualizeTracedRaysShader>(ini);
        auto params = builder.Allocate<VisualizeTracedRaysShader::Params>();
        params->View = view->view_common_params_;
        params->TracedRaysOriginBuffer = view->debug_buffers_.traced_ray_origins.Raw();
        params->TracedRaysDirectionBuffer = view->debug_buffers_.traced_ray_directions.Raw();
        params->TracedRaysStateBuffer = view->debug_buffers_.traced_ray_states.Raw();
        params->TracedRaysColorBuffer = view->debug_buffers_.traced_ray_colors.Raw();
        params->DebugOutput = view->debug_views_.visualize_traced_rays_output_.Raw();
        params->Depth = view->g_buffer_->G_depth_.Raw();
        auto cmd = Helpers::SpawnDrawIndirectCommand(builder, 2, view->debug_buffers_.traced_ray_count.Raw());
        builder.AddPass<VisualizeTracedRaysShader>({}, shader, params,
            [shader, params, dcmd = cmd.Raw()](RDGPass * pass, RHICommandQueueGraphics & queue) {
            if (auto ctx = RDGCommandHelper::BindGraphicsShader<VisualizeTracedRaysShader>(queue, pass, shader, params)) {
                queue.BeginRendering();
                queue.DrawIndirect(dcmd->GetRHI(), 1);
                queue.EndRendering();
            }
        })->AddBufferH(cmd.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
    }
    // Visualize world cache
    {
        view->debug_views_.visualize_world_cache_output_ = builder.CreateTexture2D(view->film_width_, view->film_height_,
            PixelFormatType::kR16G16B16A16_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource
            | RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer);
        auto shader = RDGShaderLibrary::Get().GetShader<VisualizeWorldCacheShader>();
        auto params = builder.Allocate<VisualizeWorldCacheShader::Params>();
        params->View = view->view_common_params_;
        params->G_Depth = view->g_buffer_->G_depth_.Raw();
        params->PreviousShadedDiffuseRadianceWithoutEmission = view->persistent_data_->prev_shaded_radiance_no_emission_.Raw();
        params->RWDebugOutputTexture = view->debug_views_.visualize_world_cache_output_.Raw();
        FillParametersForHashGridCache(view, params);
        auto HashGrids_UB = builder.Allocate<HashGridWorldCacheUB>();
        FillUniformBufferForHashGridCache(view, HashGrids_UB);
        params->HashGrids_UB = HashGrids_UB;
        Helpers::AddComputePass(builder, shader, params, DivideAndRoundUp(view->film_width_, 8), DivideAndRoundUp(view->film_height_, 8));
    }
    // Visualize spatial positions
    {
        auto dbg_persistent = view->persistent_data_->debug_persistent_data_;
        if (view->debug_buffers_.visualize_spatial_positions && view->debug_buffers_.visualize_spatial_positions_count) {
            dbg_persistent->visualize_spatial_positions_ = view->debug_buffers_.visualize_spatial_positions;
            dbg_persistent->visualize_spatial_positions_->SetExport();
            dbg_persistent->visualize_spatial_positions_count_ = view->debug_buffers_.visualize_spatial_positions_count;
            dbg_persistent->visualize_spatial_positions_count_->SetExport();
        }
        view->debug_views_.visualize_spatial_positions_output_ = builder.CreateTexture2D(view->film_width_, view->film_height_,
            PixelFormatType::kR16G16B16A16_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource
            | RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer);
        // Copy radiance to debug output first
        Helpers::CopyTexture(builder, view->radiance_.Raw(), view->debug_views_.visualize_spatial_positions_output_.Raw());
        auto shader = RDGShaderLibrary::Get().GetShader<VisualizeSpatialPositionsShader>();
        auto params = builder.Allocate<VisualizeSpatialPositionsShader::Params>();
        params->View = view->view_common_params_;
        params->G_Depth = view->g_buffer_->G_depth_.Raw();
        params->SpatialPositionsCount = dbg_persistent->visualize_spatial_positions_count_.Raw();
        params->SpatialPositionsBuffer = dbg_persistent->visualize_spatial_positions_.Raw();
        params->RWDebugOutputTexture = view->debug_views_.visualize_spatial_positions_output_.Raw();
        params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
        auto indirect_cmd = Helpers::SpawnDispatchIndirectCommand1D(
            builder, dbg_persistent->visualize_spatial_positions_count_.Raw(), VisualizeSpatialPositionsShader::kThreadGroupSize);
        Helpers::AddComputeIndirectPass<VisualizeSpatialPositionsShader>(
            builder, shader, params, indirect_cmd.Raw());
    }
    if (CVar_DebugViewMode.Get() == 0) {
        Helpers::CopyTexture(builder, view->debug_views_.visualize_ray_tracing_scene_output_.Raw(), view->debug_output_.Raw());
    } else if (CVar_DebugViewMode.Get() == 1) {
        Helpers::CopyTexture(builder, view->debug_views_.visualize_traced_rays_output_.Raw(), view->debug_output_.Raw());
    } else if (CVar_DebugViewMode.Get() == 2) {
        Helpers::CopyTexture(builder, view->debug_views_.visualize_world_cache_output_.Raw(), view->debug_output_.Raw());
    } else if (CVar_DebugViewMode.Get() == 3) {
        Helpers::CopyTexture(builder, view->volume_primitives_->volume_representative_depth_and_variation_.Raw(), view->debug_output_.Raw());
    } else if (CVar_DebugViewMode.Get() == 4) {
        Helpers::CopyTexture(builder, view->debug_views_.visualize_spatial_positions_output_.Raw(), view->debug_output_.Raw());
    }
}


MI_NAMESPACE_END