/*
 * Created: 2025/9/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <rdg/rdg.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_renderer.h>
#include <renderer/util/scan_sum.h>
#include "r_view_common.h"
#include "r_diffuse_direct_lighting.h"
#include "r_diffuse_indirect_lighting.h"
#include "r_persistent.h"

MI_NAMESPACE_BEGIN
static CVar<float> CVar_ProbeSearchSize(
    "r.diffuse_indirect_lighting.probe_reprojection_search_size",
    "Size (in pixels) of the search region when reprojecting probes from the previous frame.",
    2.f
);

static CVar<bool> CVar_ScreenProbesRayImportanceSampling(
    "r.diffuse_indirect_lighting.screen_probes_ray_importance_sampling",
    "Whether to use importance sampling when generating probe update rays. If disabled, uniform hemisphere sampling will be used.",
    true
);

static CVar<bool> CVar_ScreenProbesRayFreezeSeed(
    "r.diffuse_indirect_lighting.screen_probes_ray_freeze_seed",
    "Whether to freeze the random seed for probe update rays. This is used for debugging only.",
    false
);

static CVar<bool> CVar_ResetDiffuseIndirectLighting(
    "r.diffuse_indirect_lighting.reset",
    "Reset the diffuse indirect lighting system. This will clear all cache and reinitialize.",
    false
);

static CVar<bool> CVar_AdaptiveProbeUpdateRayAllocation(
    "r.diffuse_indirect_lighting.adaptive_probe_update_ray_allocation",
    "Whether to adaptively allocate probe update rays based on the reprojection results. If disabled, a fixed number of rays will be used for all probes.",
    true
);

static CVar<bool> CVar_Debug_OutputProbeUpdateRays(
    "r.diffuse_indirect_lighting.debug.output_probe_update_rays",
    "Output the probe update rays for debugging purposes.",
    false
);

static CVar<bool> CVar_EnableSpatialProbeFiltering(
    "r.diffuse_indirect_lighting.enable_spatial_probe_filtering",
    "Enable spatial filtering of probes to reduce noise and make the primary cache converge faster.",
    true
);

struct DiffuseIndirectLightingUB {
    uint32_t MaxNumUpdateRays;
    uint32_t HeaderTileDimension;
    glm::uvec2 TileDimensions;

    glm::vec2 InvTileDimensions;
    uint32_t  TileCount;
    uint32_t  ResetCache;

    float ProbeReprojectionSearchSize;
    uint32_t  MaxProbesToSpawnPerFrame;
    glm::vec2 InvProbeAtlasDimensions;

    uint32_t FrameIndex;
    uint32_t ProbeUpdateRaysNoImportanceSampling;
    uint32_t ProbeHeaderIndexMipLevelCount;
    uint32_t ProbeUpdateRaySampleSeed;

    uint32_t ProbeUpdateRaysNoAdaptiveAllocation;
    uint32_t ProbeSpawnSubTileJitterSeed;
    uint32_t TileProbeSpawnSeed;
    uint32_t EnableSpatialProbeFiltering;
};

BEGIN_SHADER_PARAMETERS(DiffuseIndirectLightingParams)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)

    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousScreenProbeRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeVerticalFilteredRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeFilteredRadianceDepthTexture)
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
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayOriginScreenCoordsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayAllocator)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayResultBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayInvPdfBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_Normal)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousDepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousNormalTexture)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDiffuseIndirectLightingTexture)

    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)

    SHADER_UNIFORM_BUFFER(DiffuseIndirectLightingUB, UB)
    SHADER_UNIFORM_BUFFER(DebugCommonShaderParameters, Debug)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRaysCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayOrigins)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayDirections)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayStates)


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

class ClipUpdateRayCountShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClipUpdateRayCountShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ClipUpdateRayCount");

// Trace update rayus...

class UpdateScreenProbesAndCacheShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {"DEBUG_OUTPUT_TRACED_RAY"};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateScreenProbesAndCacheShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "UpdateScreenProbesAndCache");

class FilterScreenProbesShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(FilterScreenProbesShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "FilterScreenProbes");

class WriteBackFilteredScreenProbesShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(WriteBackFilteredScreenProbesShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "WriteBackFilteredScreenProbes");

class UpdateScreenProbeCacheMRUQueueShader : public DiffuseIndirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
    DECLARE_SHADER(DiffuseIndirectLightingShader)
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {"FIRST_PASS_VERTICAL_FILTER_DIRECTION"};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateScreenProbeCacheMRUQueueShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "UpdateScreenProbeCacheMRUQueue");


class MakeTileScreenProbeHeaderIndexShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWInTileScreenProbeHeaderTexture)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWOutTileScreenProbeHeaderTexture)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER(MakeTileScreenProbeHeaderIndexShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "MakeTileScreenProbeHeaderIndex");

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

bool DiffuseIndirectLightingPersistentData::MakeSureExists(RenderGraphBuilder & builder, glm::uvec2 tile_dimensions, uint32_t header_tile_dimension) {
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
            header_tile_dimension, header_tile_dimension, PixelFormatType::kR32_UINT
        );
        TileScreenProbeHeaderTexture->SetName("TileScreenProbeHeader");
        TileScreenProbeHeaderTexture->SetExport();
        flag = false;
    }
    if (!ScreenProbeCacheMRUFlagBuffer) {
        ScreenProbeCacheMRUFlagBuffer = builder.CreateBuffer(
            RHIBufferUsageFlagBits::kStorage,
            tile_dimensions.x * tile_dimensions.y * sizeof(uint32_t)
        );
        ScreenProbeCacheMRUFlagBuffer->SetName("ScreenProbeCacheMRUFlag");
        ScreenProbeCacheMRUFlagBuffer->SetExport();
        flag = false;
    }
    return flag;
}

void Renderer::Render_ComputeIndirectDiffuseLighting(RendererView * view, RenderGraphBuilder & builder) {


    auto ini = RDGShaderInitializationInfo {};
    auto & lib = RDGShaderLibrary::Get();

    if (!view->persistent_data_->diffuse_indirect_lighting_persistent_data_) {
        view->persistent_data_->diffuse_indirect_lighting_persistent_data_ = new DiffuseIndirectLightingPersistentData();
    }
    bool need_reset = false;
    mi_check(view->film_width_ % DiffuseIndirectLightingShader::kTileSize == 0, "View width not a multiple of tile size");
    mi_check(view->film_height_ % DiffuseIndirectLightingShader::kTileSize == 0, "View height not a multiple of tile size");
    auto tile_dimensions = glm::uvec2(
        DivideAndRoundUp(view->film_width_, DiffuseIndirectLightingShader::kTileSize),
        DivideAndRoundUp(view->film_height_, DiffuseIndirectLightingShader::kTileSize)
    );
    uint32_t tile_index_mip_levels = 0;
    while ((1u << tile_index_mip_levels) < std::max(tile_dimensions.x, tile_dimensions.y))
        tile_index_mip_levels++;
    auto tile_screen_probe_header_texture = builder.CreateTexture2D(
        1 << tile_index_mip_levels, 1 << tile_index_mip_levels, PixelFormatType::kR32_UINT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess,
        tile_index_mip_levels
    );
    uint32_t header_tile_dimension = 1 << tile_index_mip_levels;

    if (!view->persistent_data_->diffuse_indirect_lighting_persistent_data_->MakeSureExists(builder, tile_dimensions, header_tile_dimension))
        need_reset = true;
    need_reset |= CVar_ResetDiffuseIndirectLighting.Get();

    auto atlas_dimensions = tile_dimensions * DiffuseIndirectLightingShader::kTileSize;
    auto screen_probe_radiance_depth = builder.CreateTexture2D(
        atlas_dimensions.x, atlas_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_vertical_filtered_radiance_depth = builder.CreateTexture2D(
        atlas_dimensions.x, atlas_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_filtered_radiance_depth = builder.CreateTexture2D(
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
    auto screen_probe_cache_mru_flag_prefix_sum_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_tiles * sizeof(uint32_t)
    );
    auto screen_probe_cache_mru_queue_entry_allocator = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
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

    uint32_t max_num_update_rays = num_tiles * 64;

    auto screen_probe_update_ray_direction_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_update_rays * sizeof(uint32_t)
    );
    auto screen_probe_update_ray_state_buffer = builder.CreateBuffer(
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
        params->RWScreenProbeVerticalFilteredRadianceDepthTexture =
            screen_probe_vertical_filtered_radiance_depth.Raw();
        params->RWScreenProbeFilteredRadianceDepthTexture =
            screen_probe_filtered_radiance_depth.Raw();
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
            view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeCacheMRUFlagBuffer.Raw();
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
        params->RWScreenProbeUpdateRayStateBuffer =
            screen_probe_update_ray_state_buffer.Raw();
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

        params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;

        auto UB = builder.Allocate<DiffuseIndirectLightingUB>();
        {
            UB->MaxNumUpdateRays = max_num_update_rays;
            UB->HeaderTileDimension = header_tile_dimension;
            UB->TileDimensions = tile_dimensions;
            UB->InvTileDimensions = 1.0f / glm::vec2(tile_dimensions);

            UB->TileCount = num_tiles;
            UB->ResetCache = need_reset;

            UB->ProbeReprojectionSearchSize = CVar_ProbeSearchSize.Get();
            UB->MaxProbesToSpawnPerFrame = num_tiles;
            UB->InvProbeAtlasDimensions = 1.0f / glm::vec2(atlas_dimensions);

            UB->FrameIndex = view->persistent_data_->frame_index_;
            UB->ProbeUpdateRaysNoImportanceSampling =
                CVar_ScreenProbesRayImportanceSampling.Get() ? 0 : 1;
            UB->ProbeHeaderIndexMipLevelCount = tile_index_mip_levels;
            UB->ProbeUpdateRaySampleSeed =
                CVar_ScreenProbesRayFreezeSeed.Get() ? 0 : (view->persistent_data_->frame_index_ + 7198272u);

            UB->ProbeUpdateRaysNoAdaptiveAllocation = CVar_AdaptiveProbeUpdateRayAllocation.Get() ? 1 : 0;
            UB->ProbeSpawnSubTileJitterSeed =
                CVar_ScreenProbesRayFreezeSeed.Get() ? 0 : view->persistent_data_->frame_index_;
            UB->TileProbeSpawnSeed =
                CVar_ScreenProbesRayFreezeSeed.Get() ? 0 : view->persistent_data_->frame_index_;
            UB->EnableSpatialProbeFiltering = CVar_EnableSpatialProbeFiltering.Get() ? 1 : 0;
        }
        params->UB = UB;
        params->Debug = view->debug_common_params_;
    }
    if (CVar_Debug_OutputProbeUpdateRays.Get()) {
        view->debug_buffers_.CreateTracedRayBuffers(builder, 256);
        params->RWDebugTracedRaysCount = view->debug_buffers_.traced_ray_count.Raw();
        params->RWDebugTracedRayOrigins = view->debug_buffers_.traced_ray_origins.Raw();
        params->RWDebugTracedRayDirections = view->debug_buffers_.traced_ray_directions.Raw();
        params->RWDebugTracedRayStates = view->debug_buffers_.traced_ray_states.Raw();
    } else {
        params->RWDebugTracedRaysCount = nullptr;
        params->RWDebugTracedRayOrigins = nullptr;
        params->RWDebugTracedRayDirections = nullptr;
        params->RWDebugTracedRayStates = nullptr;
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
            builder, shader, params, tile_dimensions.x, tile_dimensions.y
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
        builder, screen_probe_spawn_count.Raw(), 1
    );
    {
        auto shader = lib.GetShader<ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries_Shader>(ini);
        Helpers::AddComputeIndirectPass<ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries_Shader>(
            builder, shader, params,
            spawn_list_command.Raw()
        );
    }
    {
        auto shader = lib.GetShader<ClipUpdateRayCountShader>(ini);
        Helpers::AddComputePass<ClipUpdateRayCountShader>(builder, shader, params);
    }

    // Ray tracing...
    // TODO screen space tracing
    Render_HardwareRadianceRayTracing(
        view, builder,
        screen_probe_update_ray_allocator.Raw(),
        nullptr,
        screen_probe_update_ray_direction_buffer.Raw(),
        screen_probe_update_ray_state_buffer.Raw(),
        screen_probe_update_ray_origin_screen_coords_buffer.Raw(),
        nullptr,
        nullptr,
        screen_probe_update_ray_result_buffer.Raw(),
        view->persistent_data_->frame_index_ * 137
    );

    {
        auto ini_s = ini;
        if (CVar_Debug_OutputProbeUpdateRays.Get()) ini_s.optional_macros.push_back("DEBUG_OUTPUT_TRACED_RAY");
        auto shader = lib.GetShader<UpdateScreenProbesAndCacheShader>(ini_s);
        Helpers::AddComputeIndirectPass<UpdateScreenProbesAndCacheShader>(
            builder, shader, params,
            spawn_list_command.Raw()
        );
    }

    // Filter probes
    {
        auto ini_s = ini;
        ini_s.optional_macros.push_back("FIRST_PASS_VERTICAL_FILTER_DIRECTION");
        auto shader  = lib.GetShader<FilterScreenProbesShader>(ini_s);
        Helpers::AddComputePass<FilterScreenProbesShader>(
            builder, shader, params,
            tile_dimensions.x, tile_dimensions.y
        );
    }
    {
        auto shader  = lib.GetShader<FilterScreenProbesShader>(ini);
        Helpers::AddComputePass<FilterScreenProbesShader>(
            builder, shader, params,
            tile_dimensions.x, tile_dimensions.y
        );
    }

    // Write back filtered probes to history for unstable probes (faster convergence)
    {
        auto shader = lib.GetShader<WriteBackFilteredScreenProbesShader>(ini);
        Helpers::AddComputePass<WriteBackFilteredScreenProbesShader>(
            builder, shader, params,
            tile_dimensions.x, tile_dimensions.y
        );
    }

    // Scan sum
    DeviceScanSum::AddScanSum32BitsPass(builder, num_tiles,
        view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeCacheMRUFlagBuffer.Raw(),
        screen_probe_cache_mru_flag_prefix_sum_buffer.Raw()
    );

    {
        auto shader = lib.GetShader<UpdateScreenProbeCacheMRUQueueShader>(ini);
        Helpers::AddComputePass<UpdateScreenProbeCacheMRUQueueShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    // MakeTileScreenProbeHeaderIndex
    {
        for (uint i = 1; i < tile_index_mip_levels; i++) {
            auto index_params = builder.Allocate<MakeTileScreenProbeHeaderIndexShader::Params>();
            {
                index_params->RWInTileScreenProbeHeaderTexture = tile_screen_probe_header_texture.Raw();
                index_params->RWInTileScreenProbeHeaderTexture.mip_level = i - 1;
                index_params->RWOutTileScreenProbeHeaderTexture = tile_screen_probe_header_texture.Raw();
                index_params->RWOutTileScreenProbeHeaderTexture.mip_level = i;
            }
            auto shader = lib.GetShader<MakeTileScreenProbeHeaderIndexShader>(ini);
            Helpers::AddComputePass<MakeTileScreenProbeHeaderIndexShader>(
                builder, shader, index_params,
                DivideAndRoundUp(
                    1 << (tile_index_mip_levels - i),
                    DiffuseIndirectLightingShader::kTileSize
                ),
                DivideAndRoundUp(
                    1 << (tile_index_mip_levels - i),
                    DiffuseIndirectLightingShader::kTileSize
                ),
                i
            );
        }
    }

    {
        auto shader = lib.GetShader<ComputeScreenProbeSHCoefficientsShader>(ini);
        Helpers::AddComputePass<ComputeScreenProbeSHCoefficientsShader>(
            builder, shader, params,
            tile_dimensions.x, tile_dimensions.y
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

    // Update persistent data
    {
        auto persistent = view->persistent_data_->diffuse_indirect_lighting_persistent_data_;
        persistent->ScreenProbeRadianceDepthTexture = screen_probe_radiance_depth;
        persistent->ScreenProbeRadianceDepthTexture->SetExport();
        persistent->ScreenProbeCacheMRUQueueBuffer = screen_probe_cache_updated_mru_queue_buffer;
        persistent->ScreenProbeCacheMRUQueueBuffer->SetExport();
        persistent->TileScreenProbeHeaderTexture = tile_screen_probe_header_texture;
        persistent->TileScreenProbeHeaderTexture->SetExport();
    }
}

MI_NAMESPACE_END