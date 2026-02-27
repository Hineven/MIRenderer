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
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_scene.h>
#include <renderer/mi_texture.h>
#include <renderer/mi_buffer_heap.h>
#include <renderer/r_geometry_buffer.h>

#include "r_view_common.h"
#include "r_diffuse_indirect_lighting.h"

#include "r_gaussian_radiance_field.h"
#include "r_light_structure.h"
#include "r_persistent.h"
#include "r_world_radiance_cache.h"
#include "r_directional_light.h"

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

// TODO this can introduce large artifacts when overflowing the total ray budget per frame
// It can cause ray over-allocation on certain probes, overflowing the ray budget, making some newly spawned probes
// to not being updated at all, ultimately leading to black spots in the lighting.
// Consider a better strategy or disable this option permanently.
static CVar<bool> CVar_AdaptiveProbeUpdateRayAllocation(
    "r.diffuse_indirect_lighting.adaptive_probe_update_ray_allocation",
    "Whether to adaptively allocate probe update rays based on the reprojection results. If disabled, a fixed number of rays will be used for all probes.",
    false
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

static CVar<bool> CVar_NoEnvironmentLight(
    "r.diffuse_indirect_lighting.no_environment_light",
    "Disable the environment light when updating probes.",
    false
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

    uint32_t NoEnvironmentLight;
    float GRF_EmitterIntensityScale;
    glm::uvec2 Padding;
};

BEGIN_SHADER_PARAMETERS(DiffuseIndirectLightingParams)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(DirectionalLightUniform, DirectionalLight_UB)

    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousScreenProbeRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeVerticalFilteredRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeFilteredRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeIrradianceTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeSHCoefficientsRTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeSHCoefficientsGTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeSHCoefficientsBTexture)

    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousShadedDiffuseRadianceWithoutEmission)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileScreenProbeCacheIndexListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileScreenProbeCacheIndexListLengthsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileScreenProbeCacheIndexListOffsetsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileScreenProbeCacheIndexListAllocator)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheIndexReprojectionEntryBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeCacheIndexReprojectionCount)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWScreenProbeReconstructedRadianceDepthTexture)

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
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayRadianceBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayInvPdfBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayHitResolveBucketAndCellOffsetBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayHitShadingPointAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWScreenProbeUpdateRayHitShadingPointListBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayOriginBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayTMaxBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ShadePointTransmittanceRayTransmittanceBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRaySampledLightIndexBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayContributionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointToTransmittanceRayIndexBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_Normal)
    SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousDepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousNormalTexture)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDiffuseIndirectLightingTexture)

    SHADER_UNIFORM_BUFFER(DiffuseIndirectLightingUB, UB)
    SHADER_UNIFORM_BUFFER(DebugCommonShaderParameters, Debug)

    // Hash grid
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
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileListBuffer)

    SHADER_UNIFORM_BUFFER(HashGridWorldCacheUB, HashGrids_UB)

    // Light grid
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_PrecomputedActiveLightBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ActiveLightListCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ActiveLightListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ListAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ListActiveLightListIndexBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListCdfBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListLengthBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_EnvironmentVisibilityHistoryBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_BloomFilterBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_NextBloomFilterBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_NextEnvironmentVisibilityBuffer)

    SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)

    // Geometry & Material & Lighting
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightBuffer)

    // Samplers
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)


    // Debugging
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRaysCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayOrigins)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayDirections)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayStates)


END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(DiffuseIndirectLightingParams)

namespace DiffuseIndirectLightingShaders {
    class DiffuseIndirectLightingShader : public RDGShader {
    public:
        constexpr static uint32_t kTileSize = 8;
        static std::vector<std::string> GetShaderDefaultMacros() {
            return {
                "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
                "TILE_SIZE=" + std::to_string(kTileSize)
            };
        }
        static std::vector<std::string> GetShaderOptionalMacros() {
            return GetLightStructureShaderMacros();
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

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearTileScreenProbeCacheIndexListLengthsShader,
        "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ClearTileScreenProbeCacheIndexListLengths");

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

    class AllocateTileCachedScreenProbeListsShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(AllocateTileCachedScreenProbeListsShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "AllocateTileCachedScreenProbeLists");

    class ScatterReprojectedCachedProbesToTileListShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ScatterReprojectedCachedProbesToTileListShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ScatterReprojectedCachedProbesToTileList");

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

    class UpdateScreenProbeSpawnCountShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateScreenProbeSpawnCountShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "UpdateScreenProbeSpawnCount");

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

    class ResolveHitLightingFromScreenHistoryAndSpecialEmitterShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ResolveHitLightingFromScreenHistoryAndSpecialEmitterShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ResolveHitLightingFromScreenHistoryAndSpecialEmitter");

    class SampleLightRaysForUpdateRayHitsShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(SampleLightRaysForUpdateRayHitsShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "SampleLightRaysForUpdateRayHits");

    // Trace light rays ... (stochastic transmittance rays)

    class ResolveUpdateRayHitsDirectLightingFromTraceResultShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ResolveUpdateRayHitsDirectLightingFromTraceResultShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ResolveUpdateRayHitsDirectLightingFromTraceResult");

    // Hash grid update ...

    class ResolveProbeUpdateRayRadianceFromCellsShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ResolveProbeUpdateRayRadianceFromCellsShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "ResolveProbeUpdateRayRadianceFromCells");

    class UpdateScreenProbesAndCacheShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
        static std::vector<std::string> GetShaderOptionalMacros() {
            auto macros = DiffuseIndirectLightingShader::GetShaderOptionalMacros();
            macros.push_back("DEBUG_OUTPUT_TRACED_RAY");
            return macros;
        }
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateScreenProbesAndCacheShader, "mi/renderer/shaders/DiffuseIndirectLighting.hlsl", "UpdateScreenProbesAndCache");

    class FilterScreenProbesShader : public DiffuseIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(DiffuseIndirectLightingParams)
        DECLARE_SHADER(DiffuseIndirectLightingShader)
        static std::vector<std::string> GetShaderOptionalMacros() {
            auto macros = DiffuseIndirectLightingShader::GetShaderOptionalMacros();
            macros.push_back("FIRST_PASS_VERTICAL_FILTER_DIRECTION");
            return macros;
        }
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
}

using namespace DiffuseIndirectLightingShaders;

static glm::uvec2 GetTileDimensions(RendererView * view) {
    return glm::uvec2(
        DivideAndRoundUp(view->film_width_, DiffuseIndirectLightingShader::kTileSize),
        DivideAndRoundUp(view->film_height_, DiffuseIndirectLightingShader::kTileSize)
    );
}

static uint32_t GetTileIndexMipLevels(glm::uvec2 tile_dimensions) {
    uint32_t tile_index_mip_levels = 0;
    while ((1u << tile_index_mip_levels) < std::max(tile_dimensions.x, tile_dimensions.y))
        tile_index_mip_levels++;
    return tile_index_mip_levels;
}

void DiffuseIndirectLightingData::Allocate(RenderGraphBuilder &builder, RendererView * view) {
    auto tile_dimensions = GetTileDimensions(view);
    auto atlas_dimensions = tile_dimensions * DiffuseIndirectLightingShader::kTileSize;
    screen_probe_radiance_depth = builder.CreateTexture2D(
        atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    screen_probe_radiance_depth->SetName("ScreenProbeRadianceDepth");
    auto num_tiles = tile_dimensions.x * tile_dimensions.y;
    screen_probe_cache_updated_mru_queue_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    screen_probe_cache_updated_mru_queue_buffer->SetName("ScreenProbeCacheUpdatedMRUQueue");
    auto tile_index_mip_levels = GetTileIndexMipLevels(tile_dimensions);
    tile_screen_probe_header_texture = builder.CreateTexture2D(
        1 << tile_index_mip_levels, 1 << tile_index_mip_levels, PixelFormatType::kR32_UINT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess,
        tile_index_mip_levels
    );
    tile_screen_probe_header_texture->SetName("TileScreenProbeHeader");
    radiance = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR16G16B16A16_FLOAT
    );
    radiance->SetName("DiffuseIndirectLightingRadiance");
}

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

void DiffuseIndirectLightingPersistentData::FinalUpdate(RendererView *view) {
    // Update persistent data
    if (view->diffuse_direct_lighting_) {
        ScreenProbeRadianceDepthTexture = view->diffuse_indirect_lighting_->screen_probe_radiance_depth;
        ScreenProbeRadianceDepthTexture->SetExport();
        ScreenProbeCacheMRUQueueBuffer = view->diffuse_indirect_lighting_->screen_probe_cache_updated_mru_queue_buffer;
        ScreenProbeCacheMRUQueueBuffer->SetExport();
        TileScreenProbeHeaderTexture = view->diffuse_indirect_lighting_->tile_screen_probe_header_texture;
        TileScreenProbeHeaderTexture->SetExport();
    }
}

static RDGShaderInitializationInfo GetDiffuseIndirectLightingShaderInitializationInfo() {
    auto ini = RDGShaderInitializationInfo {};
    ini.optional_macros = GetLightStructureShaderMacros();
    return ini;
}

void Renderer::Render_UpdateDiffuseIndirectLighting(RendererView * view, RenderGraphBuilder & builder) {
    RDGSectionGuard section(builder, "Render_UpdateDiffuseIndirectLighting");

    auto ini = GetDiffuseIndirectLightingShaderInitializationInfo();
    ini.optional_macros = GetLightStructureShaderMacros();
    auto & lib = RDGShaderLibrary::Get();

    if (!view->persistent_data_->diffuse_indirect_lighting_persistent_data_) {
        view->persistent_data_->diffuse_indirect_lighting_persistent_data_ = new DiffuseIndirectLightingPersistentData();
    }
    bool need_reset = false;
    mi_check(view->film_width_ % DiffuseIndirectLightingShader::kTileSize == 0, "View width not a multiple of tile size");
    mi_check(view->film_height_ % DiffuseIndirectLightingShader::kTileSize == 0, "View height not a multiple of tile size");
    auto tile_dimensions = GetTileDimensions(view);
    auto tile_index_mip_levels = GetTileIndexMipLevels(tile_dimensions);

    uint32_t header_tile_dimension = 1 << tile_index_mip_levels;

    if (!view->persistent_data_->diffuse_indirect_lighting_persistent_data_
        ->MakeSureExists(builder, tile_dimensions, header_tile_dimension))
        need_reset = true;
    need_reset |= CVar_ResetDiffuseIndirectLighting.Get();

    auto atlas_dimensions = tile_dimensions * DiffuseIndirectLightingShader::kTileSize;
    auto screen_probe_vertical_filtered_radiance_depth = builder.CreateTexture2D(
        atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_filtered_radiance_depth = builder.CreateTexture2D(
        atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto sh_coeff_atlas_dimensions = glm::uvec2{tile_dimensions.x * 2, tile_dimensions.y};
    auto screen_probe_irradiance = builder.CreateTexture2D(
        tile_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_sh_coefficients_r = builder.CreateTexture2D(
        sh_coeff_atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_sh_coefficients_g = builder.CreateTexture2D(
        sh_coeff_atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    auto screen_probe_sh_coefficients_b = builder.CreateTexture2D(
        sh_coeff_atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );

    auto num_tiles = tile_dimensions.x * tile_dimensions.y;
    auto tile_screen_probe_cache_index_list_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    auto tile_screen_probe_cache_index_list_lengths_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    auto tile_screen_probe_cache_index_list_offsets_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    auto tile_screen_probe_cache_index_list_allocator = builder.CreateBuffer<uint32_t>();

    auto screen_probe_cache_index_reprojection_entry_buffer = builder.CreateBuffer<glm::uvec4>(num_tiles);
    auto screen_probe_cache_index_reprojection_count = builder.CreateBuffer<uint32_t>();

    auto screen_probe_reconstructed_radiance_depth_texture = builder.CreateTexture2D(
        atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );

    auto screen_probe_spawn_cache_matches_buffer = builder.CreateBuffer<glm::uvec2>(num_tiles);
    auto screen_probe_cache_to_mru_queue_index_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    auto screen_probe_cache_mru_flag_prefix_sum_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    auto screen_probe_cache_mru_queue_entry_allocator = builder.CreateBuffer<uint32_t>();

    auto reprojection_fail_tile_count = builder.CreateBuffer<uint32_t>();
    auto reprojection_fail_tile_list_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    auto screen_probe_spawn_count = builder.CreateBuffer<uint32_t>();
    auto screen_probe_spawn_list_buffer = builder.CreateBuffer<uint32_t>(num_tiles);

    auto screen_probe_update_ray_offsets_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    auto screen_probe_update_ray_counts_buffer = builder.CreateBuffer<uint32_t>(num_tiles);

    // Wave size is the default thread group size in almost all shaders.
    uint32_t wave_size = RHI::Get().GetDeviceProperties().wave_size;
    uint32_t max_num_update_rays = num_tiles * 64; // Theoretically this can be configured
    mi_check(max_num_update_rays % wave_size == 0, "Must be a multiple of wave_size");
    
    // Add validation to prevent excessive memory allocation
    const uint32_t kMaxUpdateRays = 128 * 128 * 128; // Reasonable upper limit
    max_num_update_rays = std::min(max_num_update_rays, kMaxUpdateRays);

    auto screen_probe_update_ray_direction_buffer = builder.CreateBuffer<glm::vec3>(max_num_update_rays);
    auto screen_probe_update_ray_state_buffer = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    auto screen_probe_update_ray_origin_screen_coords_buffer = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    auto screen_probe_update_ray_allocator = builder.CreateBuffer<uint32_t>();

    auto screen_probe_update_ray_result_buffer = builder.CreateBuffer<glm::uvec2>(max_num_update_rays);
    auto screen_probe_update_ray_radiance_buffer = builder.CreateBuffer<glm::uvec2>(max_num_update_rays);
    auto screen_probe_update_ray_inv_pdf_buffer = builder.CreateBuffer<float>(max_num_update_rays);
    auto screen_probe_update_ray_hit_resolve_bucket_and_cell_offset_buffer = builder.CreateBuffer<uint32_t>(max_num_update_rays);

    auto screen_probe_update_ray_hit_shading_point_allocator = builder.CreateBuffer<uint32_t>();
    auto screen_probe_update_ray_hit_shading_point_list_buffer = builder.CreateBuffer<uint32_t>(max_num_update_rays);

    auto shade_point_transmittance_ray_allocator = builder.CreateBuffer<uint32_t>();
    auto shade_point_transmittance_ray_direction = builder.CreateBuffer<glm::vec3>(max_num_update_rays);
    auto shade_point_transmittance_ray_origin = builder.CreateBuffer<glm::vec3>(max_num_update_rays);
    auto shade_point_transmittance_ray_state = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    auto shade_point_transmittance_ray_tmax = builder.CreateBuffer<float>(max_num_update_rays);
    auto shade_point_transmittance_ray_transmittance = builder.CreateBuffer<float>(max_num_update_rays);
    auto shade_point_transmittance_ray_sampled_light_index = builder.CreateBuffer<uint32_t>(max_num_update_rays);

    auto shade_point_transmittance_ray_contribution = builder.CreateBuffer<glm::uvec2>(max_num_update_rays);
    auto shade_point_to_transmittance_ray_index = builder.CreateBuffer<uint32_t>(max_num_update_rays);

    auto params = builder.Allocate<DiffuseIndirectLightingParams>();
    {
        params->View = view->view_common_params_;
        params->PreviousScreenProbeRadianceDepthTexture =
            view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeRadianceDepthTexture.Raw();
        params->RWScreenProbeRadianceDepthTexture =
            view->diffuse_indirect_lighting_->screen_probe_radiance_depth.Raw();
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

        params->PreviousShadedDiffuseRadianceWithoutEmission =
            view->persistent_data_->prev_shaded_radiance_no_emission_.Raw();

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

        params->RWScreenProbeReconstructedRadianceDepthTexture =
            screen_probe_reconstructed_radiance_depth_texture.Raw();

        params->RWScreenProbeSpawnCacheMatchesBuffer =
            screen_probe_spawn_cache_matches_buffer.Raw();
        params->RWScreenProbeCacheMRUQueueBuffer =
            view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeCacheMRUQueueBuffer.Raw();
        params->RWScreenProbeCacheUpdatedMRUQueueBuffer =
            view->diffuse_indirect_lighting_->screen_probe_cache_updated_mru_queue_buffer.Raw();
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
            view->diffuse_indirect_lighting_->tile_screen_probe_header_texture.Raw();
        params->TileScreenProbeHeaderTexture =
            view->diffuse_indirect_lighting_->tile_screen_probe_header_texture.Raw();

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
        params->RWScreenProbeUpdateRayRadianceBuffer =
            screen_probe_update_ray_radiance_buffer.Raw();
        params->RWScreenProbeUpdateRayInvPdfBuffer =
            screen_probe_update_ray_inv_pdf_buffer.Raw();
        params->RWScreenProbeUpdateRayHitResolveBucketAndCellOffsetBuffer =
            screen_probe_update_ray_hit_resolve_bucket_and_cell_offset_buffer.Raw();

        params->RWScreenProbeUpdateRayHitShadingPointAllocator =
            screen_probe_update_ray_hit_shading_point_allocator.Raw();
        params->RWScreenProbeUpdateRayHitShadingPointListBuffer =
            screen_probe_update_ray_hit_shading_point_list_buffer.Raw();

        params->RWShadePointTransmittanceRayAllocator =
            shade_point_transmittance_ray_allocator.Raw();
        params->RWShadePointTransmittanceRayDirectionBuffer =
            shade_point_transmittance_ray_direction.Raw();
        params->RWShadePointTransmittanceRayOriginBuffer =
            shade_point_transmittance_ray_origin.Raw();
        params->RWShadePointTransmittanceRayStateBuffer =
            shade_point_transmittance_ray_state.Raw();
        params->RWShadePointTransmittanceRayTMaxBuffer =
            shade_point_transmittance_ray_tmax.Raw();
        params->ShadePointTransmittanceRayTransmittanceBuffer =
            shade_point_transmittance_ray_transmittance.Raw();

        params->RWShadePointTransmittanceRaySampledLightIndexBuffer =
            shade_point_transmittance_ray_sampled_light_index.Raw();

        params->RWShadePointTransmittanceRayContributionBuffer =
            shade_point_transmittance_ray_contribution.Raw();
        params->RWShadePointToTransmittanceRayIndexBuffer =
            shade_point_to_transmittance_ray_index.Raw();

        params->G_Depth = view->g_buffer_->G_depth_.Raw();
        params->G_Normal = view->g_buffer_->G_normal_.Raw();
        if (view->scene_->GetSkyTexture()) {
            params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
        } else {
            params->EnvironmentMap = nullptr;
        }
        params->PreviousDepthTexture =
            view->persistent_data_->g_buffer_data_->prev_G_depth_.Raw();
        params->PreviousNormalTexture =
            view->persistent_data_->g_buffer_data_->prev_G_normal_.Raw();

        params->RWDiffuseIndirectLightingTexture =
            view->diffuse_indirect_lighting_->radiance.Raw();

        params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

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

            UB->ProbeUpdateRaysNoAdaptiveAllocation = CVar_AdaptiveProbeUpdateRayAllocation.Get() ? 0 : 1;
            UB->ProbeSpawnSubTileJitterSeed =
                CVar_ScreenProbesRayFreezeSeed.Get() ? 0 : view->persistent_data_->frame_index_;
            UB->TileProbeSpawnSeed =
                CVar_ScreenProbesRayFreezeSeed.Get() ? 0 : view->persistent_data_->frame_index_;
            UB->EnableSpatialProbeFiltering = CVar_EnableSpatialProbeFiltering.Get() ? 1 : 0;

            UB->NoEnvironmentLight = CVar_NoEnvironmentLight.Get() ? 1 : 0;
            UB->GRF_EmitterIntensityScale = CVar_GRF_EmitterIntensityScale.Get();
            UB->Padding = glm::uvec3{0};
        }
        params->UB = UB;
        params->Debug = view->debug_common_params_;

        // Hash grid
        {
            FillParametersForHashGridCache(view, params);
            auto HashGrid_UB = builder.Allocate<HashGridWorldCacheUB>();
            FillUniformBufferForHashGridCache(view, HashGrid_UB);
            params->HashGrids_UB = HashGrid_UB;
        }

        // Light grid
        {
            FillParametersForLightStructure(view, params);
            auto LightStructure_UB = builder.Allocate<LightStructureUB>();
            FillUniformBufferForLightStructure(view, LightStructure_UB);
            params->LightStructure_UB = LightStructure_UB;

            auto directional_light_ub = builder.Allocate<DirectionalLightUniform>();
            FillUniformBufferForDirectionalLight(view, directional_light_ub);
            params->DirectionalLight_UB = directional_light_ub;
        }

        // Geometry & Material & Lighting
        {
            params->LightBuffer = builder.Import(device_allocator_->GetAreaLightsUberBuffer()->GetRHI());
            params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
            params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
            params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
            params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
            params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
            params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
            params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
            params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
        }
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

    view->diffuse_indirect_lighting_->shader_params = params;

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
        auto shader = lib.GetShader<AllocateTileCachedScreenProbeListsShader>(ini);
        Helpers::AddComputePass<AllocateTileCachedScreenProbeListsShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        // Actual num of threads required: ScreenProbeCacheIndexReprojectionCount.
        // Anyway num_tiles won't be a big overestimate.
        auto shader = lib.GetShader<ScatterReprojectedCachedProbesToTileListShader>(ini);
        Helpers::AddComputePass<ScatterReprojectedCachedProbesToTileListShader>(
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
        auto shader = lib.GetShader<UpdateScreenProbeSpawnCountShader>(ini);
        Helpers::AddComputePass<UpdateScreenProbeSpawnCountShader>(
            builder, shader, params
        );
    }
    view->diffuse_indirect_lighting_->spawn_list_command = Helpers::SpawnDispatchIndirectCommand1D(
        builder, screen_probe_spawn_count.Raw(), 1
    );
    {
        auto shader = lib.GetShader<ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries_Shader>(ini);
        Helpers::AddComputeIndirectPass<ReconstructRadiance_SampleSpawnScreenProbeUpdateRays_LocateCacheEntries_Shader>(
            builder, shader, params,
            view->diffuse_indirect_lighting_->spawn_list_command.Raw()
        );
    }
    {
        auto shader = lib.GetShader<ClipUpdateRayCountShader>(ini);
        Helpers::AddComputePass<ClipUpdateRayCountShader>(builder, shader, params);
    }

    // Ray tracing...
    // TODO screen space tracing
    Render_HardwareVisibilityRayTracing(
        view, builder,
        screen_probe_update_ray_allocator.Raw(),
        nullptr,
        screen_probe_update_ray_direction_buffer.Raw(),
        screen_probe_update_ray_state_buffer.Raw(),
        screen_probe_update_ray_origin_screen_coords_buffer.Raw(),
        nullptr,
        nullptr,
        screen_probe_update_ray_result_buffer.Raw(),
        view->persistent_data_->frame_index_ * 718 + 21,
        VisibilityTraceType::kCoarseWithExactVolumeScattering // Coarse visibility will be okay
    );

    {
        auto shader = lib.GetShader<ResolveHitLightingFromScreenHistoryAndSpecialEmitterShader>(ini);
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(
            builder, screen_probe_update_ray_allocator.Raw(), wave_size
        );
        Helpers::AddComputeIndirectPass<ResolveHitLightingFromScreenHistoryAndSpecialEmitterShader>(
            builder, shader, params, cmd.Raw()
        );
    }

    view->diffuse_indirect_lighting_->shading_point_command = Helpers::SpawnDispatchIndirectCommand1D(
        builder, screen_probe_update_ray_hit_shading_point_allocator.Raw(), wave_size
    );
    {
        auto shader = lib.GetShader<SampleLightRaysForUpdateRayHitsShader>(ini);
        Helpers::AddComputeIndirectPass<SampleLightRaysForUpdateRayHitsShader>(
            builder, shader, params, view->diffuse_indirect_lighting_->shading_point_command.Raw()
        );
    }

    Render_HardwareTransmittanceRayTracing(
        view, builder,
        shade_point_transmittance_ray_allocator.Raw(),
        nullptr,
        shade_point_transmittance_ray_direction.Raw(),
        shade_point_transmittance_ray_state.Raw(),
        nullptr,
        shade_point_transmittance_ray_origin.Raw(),
        shade_point_transmittance_ray_tmax.Raw(),
        shade_point_transmittance_ray_transmittance.Raw()
    );

    {
        auto shader = lib.GetShader<ResolveUpdateRayHitsDirectLightingFromTraceResultShader>(ini);
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(
            builder, screen_probe_update_ray_hit_shading_point_allocator.Raw(), wave_size
        );
        Helpers::AddComputeIndirectPass<ResolveUpdateRayHitsDirectLightingFromTraceResultShader>(
            builder, shader, params, cmd.Raw()
        );
    }

}

void Renderer::Render_FinishDiffuseIndirectLighting(RendererView * view, RenderGraphBuilder & builder) {
    RDGSectionGuard section(builder, "Render_FinishDiffuseIndirectLighting");
    auto & lib = RDGShaderLibrary::Get();
    auto ini = GetDiffuseIndirectLightingShaderInitializationInfo();
    auto params = view->diffuse_indirect_lighting_->shader_params;
    {
        auto shader = lib.GetShader<ResolveProbeUpdateRayRadianceFromCellsShader>(ini);
        Helpers::AddComputeIndirectPass<ResolveProbeUpdateRayRadianceFromCellsShader>(
            builder, shader, params, view->diffuse_indirect_lighting_->shading_point_command.Raw()
        );
    }

    {
        auto ini_s = ini;
        if (CVar_Debug_OutputProbeUpdateRays.Get()) ini_s.optional_macros.push_back("DEBUG_OUTPUT_TRACED_RAY");
        auto shader = lib.GetShader<UpdateScreenProbesAndCacheShader>(ini_s);
        Helpers::AddComputeIndirectPass<UpdateScreenProbesAndCacheShader>(
            builder, shader, params,
            view->diffuse_indirect_lighting_->spawn_list_command.Raw()
        );
    }

    // Filter probes
    auto tile_dimensions = GetTileDimensions(view);
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
    auto num_tiles = tile_dimensions.x * tile_dimensions.y;
    auto tile_index_mip_levels = GetTileIndexMipLevels(tile_dimensions);
    // Scan sum
    DeviceScanSum::AddScanSum32BitsPass(builder, num_tiles,
        view->persistent_data_->diffuse_indirect_lighting_persistent_data_->ScreenProbeCacheMRUFlagBuffer.Raw(),
        params->RWScreenProbeCacheMRUFlagPrefixSumBuffer
    );

    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
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
                index_params->RWInTileScreenProbeHeaderTexture = view->diffuse_indirect_lighting_->tile_screen_probe_header_texture.Raw();
                index_params->RWInTileScreenProbeHeaderTexture.mip_level = i - 1;
                index_params->RWOutTileScreenProbeHeaderTexture = view->diffuse_indirect_lighting_->tile_screen_probe_header_texture.Raw();
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
}

MI_NAMESPACE_END
