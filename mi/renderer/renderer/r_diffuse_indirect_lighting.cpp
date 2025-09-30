/*
 * Created: 2025/9/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <rdg/rdg.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_renderer.h>
#include "r_view_common.h"

MI_NAMESPACE_BEGIN

struct DiffuseIndirectLightingUB {
    glm::uvec2 Unused;
    glm::uvec2 TileDimensions;

    glm::vec2 InvTileDimensions;
    uint32_t  TileCount;
    float InvTileCount;

    float ProbeReprojectionSearchSize;
    uint32_t  MaxProbesToSpawnPerFrame;
    glm::vec2 InvProbeAtlasDimensions;

    uint32_t FrameIndex;
    uint32_t ProbeUpdateRaysNoImportanceSampling;
    uint32_t ProbeHeaderIndexMipLevelCount;
    uint32_t ProbeUpdateRaySampleSeed;
};

BEGIN_SHADER_PARAMETERS(DiffuseIndirectLightingParams)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)

    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousScreenProbeRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeIrradianceTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeSHCoefficientsRTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeSHCoefficientsGTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeSHCoefficientsBTexture)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileScreenProbeCacheIndexListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileScreenProbeCacheIndexListLengthsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileScreenProbeCacheIndexListOffsetsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileScreenProbeCacheIndexListAllocator)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheIndexReprojectionEntryBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheIndexReprojectionCount)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeReconstructedRadianceDepthBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeSpawnCacheMatchesBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheMRUQueueBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheUpdatedMRUQueueBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheToMRUQueueIndexBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheMRUFlagBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheMRUFlagPrefixSumBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheMRUQueueEntryAllocator)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheDataBuffer)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeCacheRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousTileScreenProbeHeaderTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWTileScreenProbeHeaderTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, TileScreenProbeHeaderTexture)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWReprojectionFailTileCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWReprojectionFailTileListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeSpawnCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeSpawnListBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayOffsetsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayCountsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayOriginScreenCoordsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayAllocator)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayResultBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayInvPdfBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_Normal)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousDepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousNormalTexture)

    SHADER_RESOURCE_PARAMETER(Texture2D, RWDiffuseIndirectLightingTexture)

    SHADER_UNIFORM_BUFFER(DiffuseIndirectLightingUB, UB)

END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(DiffuseIndirectLightingParams)

class DiffuseIndirectLightingShader : public RDGShader {
public:
    constexpr static uint32_t kTileSize = 8;
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "TILE_SIZE=" + std::to_string(kTileSize)
        };
    }
    using RDGShader::RDGShader;
};

class ClearCountersShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearCountersShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ClearCounters");

class ClearTileScreenProbeCacheIndexListLengthsShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearTileScreenProbeCacheIndexListLengthsShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ClearTileScreenProbeCacheIndexListLengths");

class InitializeScreenProbeCacheShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(InitializeScreenProbeCacheShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "InitializeScreenProbeCache");

class ReprojectScreenProbesShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ReprojectScreenProbesShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ReprojectScreenProbes");

class ReprojectCachedProbesShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ReprojectCachedProbesShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ReprojectCachedProbes");

class AllocateTileScreenProbeMRUListsShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(AllocateTileScreenProbeMRUListsShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "AllocateTileScreenProbeMRULists");

class ScatterReprojectedCachedProbesToMRUListShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ScatterReprojectedCachedProbesToMRUListShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ScatterReprojectedCachedProbesToMRUList");

class SpawnScreenProbesShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(SpawnScreenProbesShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "SpawnScreenProbes");

class SubstituteScreenProbesShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(SubstituteScreenProbesShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "SubstituteScreenProbes");

class UpdateScrenProbeSpawnCountShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateScrenProbeSpawnCountShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "UpdateScreenProbeSpawnCount");

class ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries_Shader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
    ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries_Shader,
    "mi/renderer/shaders/DiffuseIndirectLighting.hlsl",
    "ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries");

// Trace update rayus...

class UpdateScreenProbesAndCacheShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateScreenProbesAndCacheShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "UpdateScreenProbesAndCache");

class UpdateScreenProbeCacheMRUQueueShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateScreenProbeCacheMRUQueueShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "UpdateScreenProbeCacheMRUQueue");

class MakeTileScreenProbeHeaderIndexShader : public RDGShader {
};

class ComputeScreenProbeSHCoefficientsShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ComputeScreenProbeSHCoefficientsShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ComputeScreenProbeSHCoefficients");

class ComputeDiffuseIndirectLightingShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ComputeDiffuseIndirectLightingShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ComputeDiffuseIndirectLighting");

struct DiffuseIndirectLightingPersistentData : public RefCounted<> {
    TRef<RDGTexture> ScreenProbeRadianceDepthTexture;
    TRef<RDGBuffer>  ScreenProbeCacheData;
    TRef<RDGTexture> ScreenProbeCacheRadianceDepthTexture;
    TRef<RDGBuffer>  ScreenProbeCacheMRUQueueBuffer;
    TRef<RDGTexture> TileScreenProbeHeaderTexture;

    bool MakeSureExists (RenderGraphBuilder & builder, glm::uvec2 tile_dimensions) {
        bool flag = true;
        auto atlas_dimensions = tile_dimensions * DiffuseIndirectLightingShader::kTileSize;
        if (!ScreenProbeRadianceDepthTexture) {
            ScreenProbeRadianceDepthTexture = builder.CreateTexture2D(
                atlas_dimensions.x, atlas_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
            );
            ScreenProbeRadianceDepthTexture->SetName("ScreenProbeRadianceDepth");
            ScreenProbeRadianceDepthTexture->SetExport();
            flag = false;
        }
        if (!ScreenProbeCacheData) {
            ScreenProbeCacheData = builder.CreateBuffer(
                RHIBufferUsageFlagBits::kStorage,
                atlas_dimensions.x * atlas_dimensions.y * sizeof(uint32_t) * 4
            );
            ScreenProbeCacheData->SetName("ScreenProbeCacheData");
            ScreenProbeCacheData->SetExport();
            flag = false;
        }
        if (!ScreenProbeCacheRadianceDepthTexture) {
            ScreenProbeCacheRadianceDepthTexture = builder.CreateTexture2D(
                atlas_dimensions.x, atlas_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
            );
            ScreenProbeCacheRadianceDepthTexture->SetName("ScreenProbeCacheRadianceDepth");
            ScreenProbeCacheRadianceDepthTexture->SetExport();
            flag = false;
        }
        if (!ScreenProbeCacheMRUQueueBuffer) {
            ScreenProbeCacheMRUQueueBuffer = builder.CreateBuffer(
                RHIBufferUsageFlagBits::kStorage,
                atlas_dimensions.x * atlas_dimensions.y * sizeof(uint32_t)
            );
            ScreenProbeCacheMRUQueueBuffer->SetName("ScreenProbeCacheMRUQueue");
            ScreenProbeCacheMRUQueueBuffer->SetExport();
            flag = false;
        }
        if (!TileScreenProbeHeaderTexture) {
            TileScreenProbeHeaderTexture = builder.CreateTexture2D(
                tile_dimensions.x, tile_dimensions.y, PixelFormatType::kR32_UINT
            );
            TileScreenProbeHeaderTexture->SetName("TileScreenProbeHeader");
            TileScreenProbeHeaderTexture->SetExport();
            flag = false;
        }
        return flag;
    }
};

void Renderer::Render_ComputeIndirectDiffuseLighting(RendererView * view, RenderGraphBuilder & builder) {


    auto ini = RDGShaderInitializationInfo {};
    auto & lib = RDGShaderLibrary::Get();

    if (!view->persistent_data_->diffuse_indirect_lighting_persistent_data_) {
        view->persistent_data_->diffuse_indirect_lighting_persistent_data_ = new DiffuseIndirectLightingPersistentData();
    }
    bool need_reset = false;
    auto tile_dimensions = glm::uvec2(
        DivideAndRoundUp(view->film_width_, DiffuseIndirectLightingShader::kTileSize),
        DivideAndRoundUp(view->film_height_, DiffuseIndirectLightingShader::kTileSize)
    );
    if (!view->persistent_data_->diffuse_indirect_lighting_persistent_data_->MakeSureExists(builder, tile_dimensions))
        need_reset = true;

    auto atlas_dimensions = tile_dimensions * DiffuseIndirectLightingShader::kTileSize;
    auto screen_probe_radiance_depth = builder.CreateTexture2D(
        atlas_dimensions.x, atlas_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_irradiance = builder.CreateTexture2D(
        tile_dimensions.x * 2, tile_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_sh_coefficients_r = builder.CreateTexture2D(
        tile_dimensions.x * 2, tile_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_sh_coefficients_g = builder.CreateTexture2D(
        tile_dimensions.x * 2, tile_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_sh_coefficients_b = builder.CreateTexture2D(
        tile_dimensions.x * 2, tile_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
    );

    auto num_tiles = tile_dimensions.x * tile_dimensions.y;
    auto tile_screen_probe_cache_index_list_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto tile_screen_probe_cache_index_list_lengths_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto tile_screen_probe_cache_index_list_offsets_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto tile_screen_probe_cache_index_list_allocator = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );

    auto screen_probe_cache_index_reprojection_entry_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t) * 4
    );
    auto screen_probe_cache_index_reprojection_count = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );

    auto screen_probe_reconstructed_radiance_depth_buffer = builder.CreateTexture2D(
        atlas_dimensions.x, atlas_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
    );

    auto screen_probe_spawn_cache_matches_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t) * 2
    );
    auto screen_probe_cache_to_mru_queue_index_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto screen_probe_cache_updated_mru_queue_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto screen_probe_cache_mru_flag_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto screen_probe_cache_mru_flag_prefix_sum_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto screen_probe_cache_mru_queue_entry_allocator = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );

    auto tile_screen_probe_header_texture = builder.CreateTexture2D(
        tile_dimensions.x, tile_dimensions.y, PixelFormatType::kR32_UINT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess,
        tile_index_mip_levels
    );

    auto reprojection_fail_tile_count = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );
    auto reprojection_fail_tile_list_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto screen_probe_spawn_count = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );
    auto screen_probe_spawn_list_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );

    auto screen_probe_update_ray_offsets_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto screen_probe_update_ray_counts_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto screen_probe_update_ray_direction_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_update_rays * sizeof(uint32_t)
    );
    auto screen_probe_update_ray_origin_screen_coords_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_update_rays * sizeof(uint32_t)
    );
    auto screen_probe_update_ray_allocator = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );

    auto screen_probe_update_ray_result_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_update_rays * sizeof(uint32_t) * 2
    );
    auto screen_probe_update_ray_inv_pdf_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_update_rays * sizeof(float)
    );

    auto params = builder.Allocate<DiffuseIndirectLightingParams>();
    {
        params->View = view->view_common_params_;
        params->PreviousScreenProbeRadianceDepthTexture =
            view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeRadianceDepthTexture.Raw();
        params->RWScreenProbeRadianceDepthTexture =
            screen_probe_radiance_depth.Raw();
        params->RWScreenProbeIrradianceTexture =
            screen_probe_irradiance.Raw();
        params->RWScreenProbeSHCoefficientsRTexture =
            screen_probe_sh_coefficients_r.Raw();
        params->RWScreenProbeSHCoefficientsGTexture =
            screen_probe_sh_coefficients_g.Raw();
        params->RWScreenProbeSHCoefficientsBTexture =
            screen_probe_sh_coefficients_b.Raw();

        params->RWTileScreenProbeCacheIndexListBuffer =
            tile_screen_probe_cache_index_list_buffer.Raw();
        params->RWTileScreenProbeCacheIndexListLengthsBuffer =
            tile_screen_probe_cache_index_list_lengths_buffer.Raw();
        params->RWTileScreenProbeCacheIndexListOffsetsBuffer =
            tile_screen_probe_cache_index_list_offsets_buffer.Raw();
        params->RWTileScreenProbeCacheIndexListAllocator =
            tile_screen_probe_cache_index_list_allocator.Raw();

        params->RWScreenProbeCacheIndexReprojectionEntryBuffer =
            screen_probe_cache_index_reprojection_entry_buffer.Raw();
        params->RWScreenProbeCacheIndexReprojectionCount =
            screen_probe_cache_index_reprojection_count.Raw();

        params->RWScreenProbeReconstructedRadianceDepthBuffer =
            screen_probe_reconstructed_radiance_depth_buffer.Raw();

        params->RWScreenProbeSpawnCacheMatchesBuffer =
            screen_probe_spawn_cache_matches_buffer.Raw();
        params->RWScreenProbeCacheMRUQueueBuffer =
            view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeCacheMRUQueueBuffer.Raw();
        params->RWScreenProbeCacheUpdatedMRUQueueBuffer =
            screen_probe_cache_updated_mru_queue_buffer.Raw();
        params->RWScreenProbeCacheToMRUQueueIndexBuffer =
            screen_probe_cache_to_mru_queue_index_buffer.Raw();
        params->RWScreenProbeCacheMRUFlagBuffer =
            screen_probe_cache_mru_flag_buffer.Raw();
        params->RWScreenProbeCacheMRUFlagPrefixSumBuffer =
            screen_probe_cache_mru_flag_prefix_sum_buffer.Raw();
        params->RWScreenProbeCacheMRUQueueEntryAllocator =
            screen_probe_cache_mru_queue_entry_allocator.Raw();

        params->RWScreenProbeCacheDataBuffer =
            view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeCacheData.Raw();

        params->RWScreenProbeCacheRadianceDepthTexture =
            view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeCacheRadianceDepthTexture.Raw();
        params->PreviousTileScreenProbeHeaderTexture =
            view->persistent_data_->diffuse_indirect_lighting_persistent_data_->TileScreenProbeHeaderTexture.Raw();
        params->RWTileScreenProbeHeaderTexture =
            tile_screen_probe_header_texture.Raw();
        params->TileScreenProbeHeaderTexture =
            tile_screen_probe_header_texture.Raw();

        params->RWReprojectionFailTileCount =
            reprojection_fail_tile_count.Raw();
        params->RWReprojectionFailTileListBuffer =
            reprojection_fail_tile_list_buffer.Raw();
        params->RWScreenProbeSpawnCount =
            screen_probe_spawn_count.Raw();
        params->RWScreenProbeSpawnListBuffer =
            screen_probe_spawn_list_buffer.Raw();

        params->RWScreenProbeUpdateRayOffsetsBuffer =
            screen_probe_update_ray_offsets_buffer.Raw();
        params->RWScreenProbeUpdateRayCountsBuffer =
            screen_probe_update_ray_counts_buffer.Raw();
        params->RWScreenProbeUpdateRayDirectionBuffer =
            screen_probe_update_ray_direction_buffer.Raw();
        params->RWScreenProbeUpdateRayOriginScreenCoordsBuffer =
            screen_probe_update_ray_origin_screen_coords_buffer.Raw();
        params->RWScreenProbeUpdateRayAllocator =
            screen_probe_update_ray_allocator.Raw();

        params->RWScreenProbeUpdateRayResultBuffer =
            screen_probe_update_ray_result_buffer.Raw();
        params->RWScreenProbeUpdateRayInvPdfBuffer =
            screen_probe_update_ray_inv_pdf_buffer.Raw();

        params->G_Depth = view->G_depth_.Raw();
        params->G_Normal = view->G_normal_.Raw();
        params->PreviousDepthTexture =
            view->persistent_data_->prev_G_depth.Raw();
        params->PreviousNormalTexture =
            view->persistent_data_->prev_G_normal.Raw();

        params->RWDiffuseIndirectLightingTexture =
            view->diffuse_indirect_lighting_.Raw();

        auto UB = builder.Allocate<DiffuseIndirectLightingUB>();
        {
            UB->TileDimensions = tile_dimensions;
            UB->InvTileDimensions = 1.0f / glm::vec2(tile_dimensions);

            UB->TileCount = num_tiles;
            UB->InvTileCount = 1.0f / float(num_tiles);

            UB->ProbeReprojectionSearchSize = CVar_ProbeSearchSize.Get();
            UB->MaxProbesToSpawnPerFrame = num_tiles;
            UB->InvProbeAtlasDimensions = 1.0f / glm::vec2(atlas_dimensions);

            UB->FrameIndex = view->persistent_data_->frame_index_;
            UB->ProbeUpdateRaysNoImportanceSampling =
                CVar_ScreenProbesRayImportanceSampling.Get() ? 0 : 1;
            UB->ProbeHeaderIndexMipLevelCount = tile_index_mip_levels;
            UB->ProbeUpdateRaySampleSeed =
                CVar_ScreenProbesRayFreezeSeed.Get() ? 0 : view->persistent_data_->frame_index_;
        }
        params->UB = UB;
    }
    // Wave size is the default thread group size in almost all shaders.
    uint32_t wave_size = RHI::Get().GetDeviceProperties().wave_size;
    {
        auto shader = lib.GetShader<ClearCountersShader>(ini);
        Helpers::AddComputePass<ClearCountersShader>(builder, shader, params);
    }
    {
        auto shader = lib.GetShader<ClearTileScreenProbeCacheIndexListLengthsShader>(ini);
        Helpers::AddComputePass<ClearTileScreenProbeCacheIndexListLengthsShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    if (need_reset) {
        auto shader = lib.GetShader<InitializeScreenProbeCacheShader>(ini);
        Helpers::AddComputePass<InitializeScreenProbeCacheShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        auto shader = lib.GetShader<ReprojectScreenProbesShader>(ini);
        Helpers::AddComputePass<ReprojectScreenProbesShader>(
            builder, shader, params,
            DivideAndRoundUp(tile_dimensions.x, DiffuseIndirectLightingShader::kTileSize),
            DivideAndRoundUp(tile_dimensions.y, DiffuseIndirectLightingShader::kTileSize)
        );
    }
    {
        auto shader = lib.GetShader<ReprojectCachedProbesShader>(ini);
        Helpers::AddComputePass<ReprojectCachedProbesShader>(
            builder, shader, params,
            DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        auto shader = lib.GetShader<AllocateTileScreenProbeMRUListsShader>(ini);
        Helpers::AddComputePass<AllocateTileScreenProbeMRUListsShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        // Actual num of threads required: ScreenProbeCacheIndexReprojectionCount.
        // Anyway num_tiles won't be a big overestimate.
        auto shader = lib.GetShader<ScatterReprojectedCachedProbesToMRUListShader>(ini);
        Helpers::AddComputePass<ScatterReprojectedCachedProbesToMRUListShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        auto shader = lib.GetShader<SpawnScreenProbesShader>(ini);
        Helpers::AddComputePass<SpawnScreenProbesShader>(
            builder, shader, params,
            DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        auto command = Helpers::SpawnDispatchIndirectCommand1D(
            builder, reprojection_fail_tile_count.Raw(), wave_size
        );
        auto shader = lib.GetShader<SubstituteScreenProbesShader>(ini);
        Helpers::AddComputeIndirectPass<SubstituteScreenProbesShader>(
            builder, shader, params, command.Raw()
        );
    }
    {
        auto shader = lib.GetShader<UpdateScrenProbeSpawnCountShader>(ini);
        Helpers::AddComputePass<UpdateScrenProbeSpawnCountShader>(
            builder, shader, params
        );
    }
    auto spawn_list_command = Helpers::SpawnDispatchIndirectCommand1D(
        builder, screen_probe_spawn_count.Raw(), wave_size
    );
    {
        auto shader = lib.GetShader<ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries_Shader>(ini);
        Helpers::AddComputeIndirectPass<ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries_Shader>(
            builder, shader, params,
            spawn_list_command.Raw()
        );
    }

    // Ray tracing...
    // TODO

    {
        auto shader = lib.GetShader<UpdateScreenProbesAndCacheShader>(ini);
        Helpers::AddComputeIndirectPass<UpdateScreenProbesAndCacheShader>(
            builder, shader, params,
            spawn_list_command.Raw()
        );
    }

    // Scan sum
    // TODO

    {
        auto shader = lib.GetShader<UpdateScreenProbeCacheMRUQueueShader>(ini);
        Helpers::AddComputePass<UpdateScreenProbeCacheMRUQueueShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    // MakeTileScreenProbeHeaderIndex
    {
        // TODO
    }

    {
        auto shader = lib.GetShader<ComputeScreenProbeSHCoefficientsShader>(ini);
        Helpers::AddComputePass<ComputeScreenProbeSHCoefficientsShader>(
            builder, shader, params,
            num_tiles
        );
    }
    {
        auto shader = lib.GetShader<ComputeDiffuseIndirectLightingShader>(ini);
        Helpers::AddComputePass<ComputeDiffuseIndirectLightingShader>(
            builder, shader, params,
            DivideAndRoundUp(view->film_width_, DiffuseIndirectLightingShader::kTileSize),
            DivideAndRoundUp(view->film_height_, DiffuseIndirectLightingShader::kTileSize)
        );
    }
}

MI_NAMESPACE_END