/*
 * Created: 2025/11/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <rdg/rdg.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_renderer.h>
#include <renderer/mi_scene.h>
#include <renderer/mi_texture.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_buffer_heap.h>
#include <renderer/r_geometry_buffer.h>

#include "r_view_common.h"
#include "r_volume_indirect_lighting.h"
#include "r_gaussian_radiance_field.h"
#include "r_light_structure.h"
#include "r_persistent.h"
#include "r_volume_primitives.h"
#include "r_world_radiance_cache.h"

MI_NAMESPACE_BEGIN
    static CVar CVar_VolumeProbeSearchSize(
    "r.volume_indirect_lighting.probe_reprojection_search_size",
    "Size (in pixels) of the search region when reprojecting probes from the previous frame.",
    5.5f
);

static CVar CVar_VolumeProbeDepthSearchTransmittanceThreshold(
    "r.volume_indirect_lighting.probe_depth_search_transmittance_threshold",
    "Transmittance threshold when searching for probe depth during reprojection. ",
    0.7f
);

static CVar CVar_VolumeProbesRayImportanceSampling(
    "r.volume_indirect_lighting.volume_probes_ray_importance_sampling",
    "Whether to use importance sampling when generating probe update rays. If disabled, uniform hemisphere sampling will be used.",
    true
);

static CVar CVar_VolumeProbesRayFreezeSeed(
    "r.volume_indirect_lighting.volume_probes_ray_freeze_seed",
    "Whether to freeze the random seed for probe update rays. This is used for debugging only.",
    false
);

static CVar CVar_ResetVolumeIndirectLighting(
    "r.volume_indirect_lighting.reset",
    "Reset the volume indirect lighting system. This will clear all cache and reinitialize.",
    false
);

static CVar CVar_Debug_OutputProbeUpdateRays(
    "r.volume_indirect_lighting.debug.output_probe_update_rays",
    "Output the probe update rays for debugging purposes.",
    false
);

static CVar CVar_Debug_OutputProbePositions(
    "r.volume_indirect_lighting.debug.output_probe_positions",
    "Output the probe positions for debugging purposes.",
    false
);

static CVar CVar_NoEnvironmentLight(
    "r.volume_indirect_lighting.no_environment_light",
    "Disable the environment light when updating probes.",
    false
);

static CVar CVar_NoIndirectLighting(
    "r.volume_indirect_lighting.no_indirect_lighting",
    "Disable indirect lighting rendering (environment lighting is still active).",
    false
);

static CVar CVar_VolumeScreenReuseNoDepthTesting(
    "r.volume_indirect_lighting.screen_reuse_no_depth_testing",
    "Whether to skip screen space depth testing when reusing history radiance. (always reuse)",
    false
);

static CVar CVar_NoScreenReuseEnergyDecay(
    "r.volume_indirect_lighting.no_screen_reuse_energy_decay",
    "Disable energy decay when reusing screen space history radiance.",
    true
);

struct VolumeIndirectLightingUB {
    uint32_t MaxNumUpdateRays;
    float    GRF_EmitterIntensityScale;
    glm::uvec2 TileDimensions;

    glm::vec2 InvTileDimensions;
    uint32_t  TileCount;
    uint32_t  ResetCache;

    float ProbeReprojectionSearchSize;
    uint32_t  MaxProbesToSpawnPerFrame;
    glm::vec2 InvProbeAtlasDimensions;

    uint32_t FrameIndex;
    uint32_t ProbeUpdateRaysNoImportanceSampling;
    uint32_t ScreenReuseNoDepthTesting;
    uint32_t ProbeUpdateRaySampleSeed;

    uint32_t ProbeUpdateRaysNoAdaptiveAllocation;
    uint32_t ProbeSpawnSubTileJitterSeed;
    uint32_t TileProbeSpawnSeed;
    uint32_t NoEnvironmentLight;

    uint32_t NoIndirectLighting;
    float LnProbeDepthSearchTransmittanceThresh;
    uint32_t NoScreenReuseEnergyDecay;
    uint32_t Padding0;
};

BEGIN_SHADER_PARAMETERS(VolumeIndirectLightingParams)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)

    // SHADER_RESOURCE_PARAMETER(Texture2D, PreviousVolumeProbeRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeProbeRadianceDepthTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeProbeHeaderTexture)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeProbeIrradianceTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeProbeSHCoefficientsRTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeProbeSHCoefficientsGTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeProbeSHCoefficientsBTexture)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWSpawnedVolumeProbeHeaderBuffer)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeProbeReconstructedRadianceDepthTexture)


    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeSpawnAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeMRUQueueBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeNextMRUQueueBuffer)


    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PreviousActiveVolumeProbeCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PreviousActiveVolumeProbeListBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveVolumeProbeCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveVolumeProbeListBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileVolumeProbeIndexListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileVolumeProbeIndexListLengthsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileVolumeProbeIndexListOffsetsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileVolumeProbeIndexListAllocator)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileVolumeProbeReprojectionEntryAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileVolumeProbeReprojectionEntryBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileSpawnedVolumeProbeBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayOffsetsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayCountsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayOriginBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayAllocator)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayResultBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayRadianceBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayInvPdfBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayHitResolveBucketAndCellOffsetBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayHitShadingPointAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeProbeUpdateRayHitShadingPointListBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayOriginBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayTMaxBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ShadePointTransmittanceRayTransmittanceBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRaySampledLightIndexBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointTransmittanceRayContributionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadePointToTransmittanceRayIndexBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousNormalTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousDepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousShadedDiffuseRadianceWithoutEmission)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_VolumeSampleDepth)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_VolumeSampleColor)
    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeMinMaxTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeDensityTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousVolumeMinMaxTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousVolumeDensityTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousVolumeRadianceTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousTransmittanceTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, PreviousVolumeColorTexture)

    SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeIndirectLightingTexture)

    SHADER_UNIFORM_BUFFER(VolumeIndirectLightingUB, UB)
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

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugVolumeProbePositionsBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugVolumeProbePositionsCount)

END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(VolumeIndirectLightingParams)

namespace VolumeIndirectLightingShaders {
    class VolumeIndirectLightingShader : public RDGShader {
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

    class InitializeVolumeProbeCacheShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(InitializeVolumeProbeCacheShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "InitializeVolumeProbeCache");

    class ClearCountersShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearCountersShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ClearCounters");

    // Added shader to clear per-tile index list lengths/offsets
    class ClearTileVolumeProbeIndexListsShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };
    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearTileVolumeProbeIndexListsShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ClearTileVolumeProbeIndexLists");

    class InjectVolumeProbesShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(InjectVolumeProbesShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "InjectVolumeProbes");

    class AllocateTileVolumeProbeListsShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(AllocateTileVolumeProbeListsShader,
        "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "AllocateTileVolumeProbeLists");

    class ScatterReprojectedVolumeProbesToTileListShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ScatterReprojectedVolumeProbesToTileListShader,
        "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ScatterReprojectedVolumeProbesToTileList");

    class SpawnVolumeProbesShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(SpawnVolumeProbesShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "SpawnVolumeProbes");

    class ClipVolumeProbeSpawnAllocatorShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClipVolumeProbeSpawnAllocatorShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ClipVolumeProbeSpawnAllocator");

    class ReconstructRadiance_SampleSpawnVolumeProbeUpdateRaysShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ReconstructRadiance_SampleSpawnVolumeProbeUpdateRaysShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ReconstructRadiance_SampleSpawnVolumeProbeUpdateRays");

    class ClipUpdateRayCountShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClipUpdateRayCountShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ClipUpdateRayCount");

    class ResolveHitLightingFromScreenHistoryAndSpecialEmitterShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ResolveHitLightingFromScreenHistoryAndSpecialEmitterShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ResolveHitLightingFromScreenHistoryAndSpecialEmitter");

    class SampleLightRaysForUpdateRayHitsShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(SampleLightRaysForUpdateRayHitsShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "SampleLightRaysForUpdateRayHits");

    class ResolveUpdateRayHitsDirectLightingFromTraceResultShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ResolveUpdateRayHitsDirectLightingFromTraceResultShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ResolveUpdateRayHitsDirectLightingFromTraceResult");

    class ResolveProbeUpdateRayRadianceFromCellsShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        ResolveProbeUpdateRayRadianceFromCellsShader,
        "mi/renderer/shaders/VolumeIndirectLighting.hlsl",
        "ResolveProbeUpdateRayRadianceFromCells");

    class UpdateVolumeProbesAndCacheShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateVolumeProbesAndCacheShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "UpdateVolumeProbesAndCache");


    // Trace update rayus...

    class UpdateVolumeProbeCacheMRUQueueShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(UpdateVolumeProbeCacheMRUQueueShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "UpdateVolumeProbeCacheMRUQueue");

    class ComputeVolumeProbeSHCoefficientsShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ComputeVolumeProbeSHCoefficientsShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ComputeVolumeProbeSHCoefficients");

    class ComputeVolumeIndirectLightingShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ComputeVolumeIndirectLightingShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "ComputeVolumeIndirectLighting");

    class DebugOutputVolumeProbePositionsShader final : public VolumeIndirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeIndirectLightingParams)
        DECLARE_SHADER(VolumeIndirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(DebugOutputVolumeProbePositionsShader, "mi/renderer/shaders/VolumeIndirectLighting.hlsl", "DebugOutputVolumeProbePositions");

}

using namespace VolumeIndirectLightingShaders;

static glm::uvec2 GetTileDimensions(RendererView * view) {
    return {
        DivideAndRoundUp(view->film_width_, VolumeIndirectLightingShader::kTileSize),
        DivideAndRoundUp(view->film_height_, VolumeIndirectLightingShader::kTileSize)
    };
}

void VolumeIndirectLightingData::Allocate(RenderGraphBuilder &builder, RendererView * view) {
    auto tile_dimensions = GetTileDimensions(view);
    auto atlas_dimensions = tile_dimensions * VolumeIndirectLightingShader::kTileSize;
    active_volume_probe_count = builder.CreateBuffer<uint32_t>();
    active_volume_probe_count->SetName("ActiveVolumeProbeCount");

    auto num_tiles = tile_dimensions.x * tile_dimensions.y;
    active_volume_probe_list_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    active_volume_probe_list_buffer->SetName("ActiveVolumeProbeListBuffer");

    volume_probe_next_mru_queue_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    volume_probe_next_mru_queue_buffer->SetName("VolumeProbeNextMRUQueueBuffer");

    volume_probe_radiance_depth = builder.CreateTexture2D(atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT);
    volume_probe_radiance_depth->SetName("VolumeProbeRadianceDepth");

    radiance = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR16G16B16A16_FLOAT);
    radiance->SetName("VolumeIndirectLightingRadiance");
}

bool VolumeIndirectLightingPersistentData::MakeSureExists(RenderGraphBuilder & builder, glm::uvec2 tile_dimensions) {
    bool flag = true;
    auto atlas_dimensions = tile_dimensions * VolumeIndirectLightingShader::kTileSize;

    if (!VolumeProbeMRUQueueBuffer) {
        VolumeProbeMRUQueueBuffer = builder.CreateBuffer<uint32_t>(tile_dimensions.x * tile_dimensions.y);
        VolumeProbeMRUQueueBuffer->SetName("VolumeProbeMRUQueue");
        VolumeProbeMRUQueueBuffer->SetExport();
        flag = false;
    }
    if (!VolumeProbeRadianceDepthTexture) {
        VolumeProbeRadianceDepthTexture = builder.CreateTexture2D(
            atlas_dimensions.x, atlas_dimensions.y, PixelFormatType::kR16G16B16A16_FLOAT
        );
        VolumeProbeRadianceDepthTexture->SetName("VolumeProbeRadianceDepth");
        VolumeProbeRadianceDepthTexture->SetExport();
        flag = false;
    }
    if (!VolumeProbeHeaderTexture) {
        VolumeProbeHeaderTexture = builder.CreateTexture2D(
            tile_dimensions.x, tile_dimensions.y, PixelFormatType::kR32G32B32A32_UINT
        );
        VolumeProbeHeaderTexture->SetName("VolumeProbeHeader");
        VolumeProbeHeaderTexture->SetExport();
        flag = false;
    }
    return flag;
}

void VolumeIndirectLightingPersistentData::FinalUpdate(RendererView *view) {
    // Update persistent data
    ActiveVolumeProbeCount = view->volume_indirect_lighting_->active_volume_probe_count;
    ActiveVolumeProbeCount->SetExport();
    ActiveVolumeProbeListBuffer = view->volume_indirect_lighting_->active_volume_probe_list_buffer;
    ActiveVolumeProbeListBuffer->SetExport();
    VolumeProbeMRUQueueBuffer = view->volume_indirect_lighting_->volume_probe_next_mru_queue_buffer;
    VolumeProbeMRUQueueBuffer->SetExport();
}

static RDGShaderInitializationInfo GetVolumeIndirectLightingShaderInitializationInfo() {
    auto ini = RDGShaderInitializationInfo {};
    ini.optional_macros = GetLightStructureShaderMacros();
    return ini;
}

void Renderer::Render_UpdateVolumeIndirectLighting(RendererView * view, RenderGraphBuilder & builder) {
    RDGSectionGuard section(builder, "Render_UpdateVolumeIndirectLighting");

    auto ini = GetVolumeIndirectLightingShaderInitializationInfo();
    auto & lib = RDGShaderLibrary::Get();

    if (!view->persistent_data_->volume_indirect_lighting_persistent_data_) {
        view->persistent_data_->volume_indirect_lighting_persistent_data_ = new VolumeIndirectLightingPersistentData();
    }
    bool need_reset = false;
    mi_check(view->film_width_ % VolumeIndirectLightingShader::kTileSize == 0, "View width not a multiple of tile size");
    mi_check(view->film_height_ % VolumeIndirectLightingShader::kTileSize == 0, "View height not a multiple of tile size");
    auto tile_dimensions = GetTileDimensions(view);

    if (!view->persistent_data_->volume_indirect_lighting_persistent_data_
        ->MakeSureExists(builder, tile_dimensions))
        need_reset = true;
    need_reset |= CVar_ResetVolumeIndirectLighting.Get();

    auto atlas_dimensions = tile_dimensions * VolumeIndirectLightingShader::kTileSize;
    auto sh_coeff_atlas_dimensions = glm::uvec2{tile_dimensions.x * 2, tile_dimensions.y};
    auto volume_probe_irradiance = builder.CreateTexture2D(
        tile_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    volume_probe_irradiance->SetName("VolumeProbeIrradiance");
    auto volume_probe_sh_coefficients_r = builder.CreateTexture2D(
        sh_coeff_atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    volume_probe_sh_coefficients_r->SetName("VolumeProbeSHCoefficientsR");
    auto volume_probe_sh_coefficients_g = builder.CreateTexture2D(
        sh_coeff_atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    volume_probe_sh_coefficients_g->SetName("VolumeProbeSHCoefficientsG");
    auto volume_probe_sh_coefficients_b = builder.CreateTexture2D(
        sh_coeff_atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    volume_probe_sh_coefficients_b->SetName("VolumeProbeSHCoefficientsB");

    auto num_tiles = tile_dimensions.x * tile_dimensions.y;
    auto spawned_volume_probe_header_buffer = builder.CreateBuffer<glm::uvec4>(num_tiles);
    spawned_volume_probe_header_buffer->SetName("SpawnedVolumeProbeHeaderBuffer");
    auto volume_probe_reconstructed_radiance_depth_texture = builder.CreateTexture2D(
        atlas_dimensions, PixelFormatType::kR16G16B16A16_FLOAT
    );
    volume_probe_reconstructed_radiance_depth_texture->SetName("VolumeProbeReconstructedRadianceDepthTexture");

    auto volume_probe_spawn_allocator = builder.CreateBuffer<uint32_t>();
    volume_probe_spawn_allocator->SetName("VolumeProbeSpawnAllocator");

    auto tile_volume_probe_index_list_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    tile_volume_probe_index_list_buffer->SetName("TileVolumeProbeIndexListBuffer");
    auto tile_volume_probe_index_list_lengths_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    tile_volume_probe_index_list_lengths_buffer->SetName("TileVolumeProbeIndexListLengthsBuffer");
    auto tile_volume_probe_index_list_offsets_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    tile_volume_probe_index_list_offsets_buffer->SetName("TileVolumeProbeIndexListOffsetsBuffer");
    auto tile_volume_probe_index_list_allocator = builder.CreateBuffer<uint32_t>();
    tile_volume_probe_index_list_allocator->SetName("TileVolumeProbeIndexListAllocator");

    auto tile_volume_probe_reprojection_entry_allocator = builder.CreateBuffer<uint32_t>();
    tile_volume_probe_reprojection_entry_allocator->SetName("TileVolumeProbeReprojectionEntryAllocator");
    auto tile_volume_probe_reprojection_entry_buffer = builder.CreateBuffer<glm::uvec3>(num_tiles);
    tile_volume_probe_reprojection_entry_buffer->SetName("TileVolumeProbeReprojectionEntryBuffer");
    auto tile_spawned_volume_probe_header_buffer = builder.CreateBuffer<glm::uvec4>(num_tiles);
    tile_spawned_volume_probe_header_buffer->SetName("TileSpawnedVolumeProbeBuffer");

    auto volume_probe_update_ray_offsets_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    volume_probe_update_ray_offsets_buffer->SetName("VolumeProbeUpdateRayOffsetsBuffer");
    auto volume_probe_update_ray_counts_buffer = builder.CreateBuffer<uint32_t>(num_tiles);
    volume_probe_update_ray_counts_buffer->SetName("VolumeProbeUpdateRayCountsBuffer");

    // Wave size is the default thread group size in almost all shaders.
    uint32_t wave_size = RHI::Get().GetDeviceProperties().wave_size;
    uint32_t max_num_update_rays = num_tiles * 64; // Theoretically this can be configured
    mi_check(max_num_update_rays % wave_size == 0, "Must be a multiple of wave_size");

    // Add validation to prevent excessive memory allocation
    const uint32_t kMaxUpdateRays = 128 * 128 * 128; // Reasonable upper limit
    max_num_update_rays = std::min(max_num_update_rays, kMaxUpdateRays);

    auto volume_probe_update_ray_direction_buffer = builder.CreateBuffer<glm::vec3>(max_num_update_rays);
    volume_probe_update_ray_direction_buffer->SetName("VolumeProbeUpdateRayDirectionBuffer");
    auto volume_probe_update_ray_state_buffer = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    volume_probe_update_ray_state_buffer->SetName("VolumeProbeUpdateRayStateBuffer");
    auto volume_probe_update_ray_origin_buffer = builder.CreateBuffer<glm::vec3>(max_num_update_rays);
    volume_probe_update_ray_origin_buffer->SetName("VolumeProbeUpdateRayOriginBuffer");
    auto volume_probe_update_ray_allocator = builder.CreateBuffer<uint32_t>();
    volume_probe_update_ray_allocator->SetName("VolumeProbeUpdateRayAllocator");

    auto volume_probe_update_ray_result_buffer = builder.CreateBuffer<glm::uvec2>(max_num_update_rays);
    volume_probe_update_ray_result_buffer->SetName("VolumeProbeUpdateRayResultBuffer");
    auto volume_probe_update_ray_radiance_buffer = builder.CreateBuffer<glm::uvec2>(max_num_update_rays);
    volume_probe_update_ray_radiance_buffer->SetName("VolumeProbeUpdateRayRadianceBuffer");
    auto volume_probe_update_ray_inv_pdf_buffer = builder.CreateBuffer<float>(max_num_update_rays);
    volume_probe_update_ray_inv_pdf_buffer->SetName("VolumeProbeUpdateRayInvPdfBuffer");
    auto volume_probe_update_ray_hit_resolve_bucket_and_cell_offset_buffer = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    volume_probe_update_ray_hit_resolve_bucket_and_cell_offset_buffer->SetName("VolumeProbeUpdateRayHitResolveBucketAndCellOffsetBuffer");

    auto volume_probe_update_ray_hit_shading_point_allocator = builder.CreateBuffer<uint32_t>();
    volume_probe_update_ray_hit_shading_point_allocator->SetName("VolumeProbeUpdateRayHitShadingPointAllocator");
    auto volume_probe_update_ray_hit_shading_point_list_buffer = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    volume_probe_update_ray_hit_shading_point_list_buffer->SetName("VolumeProbeUpdateRayHitShadingPointListBuffer");

    auto shade_point_transmittance_ray_allocator = builder.CreateBuffer<uint32_t>();
    shade_point_transmittance_ray_allocator->SetName("ShadePointTransmittanceRayAllocator");
    auto shade_point_transmittance_ray_direction = builder.CreateBuffer<glm::vec3>(max_num_update_rays);
    shade_point_transmittance_ray_direction->SetName("ShadePointTransmittanceRayDirectionBuffer");
    auto shade_point_transmittance_ray_origin = builder.CreateBuffer<glm::vec3>(max_num_update_rays);
    shade_point_transmittance_ray_origin->SetName("ShadePointTransmittanceRayOriginBuffer");
    auto shade_point_transmittance_ray_state = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    shade_point_transmittance_ray_state->SetName("ShadePointTransmittanceRayStateBuffer");
    auto shade_point_transmittance_ray_tmax = builder.CreateBuffer<float>(max_num_update_rays);
    shade_point_transmittance_ray_tmax->SetName("ShadePointTransmittanceRayTMaxBuffer");
    auto shade_point_transmittance_ray_transmittance = builder.CreateBuffer<float>(max_num_update_rays);
    shade_point_transmittance_ray_transmittance->SetName("ShadePointTransmittanceRayTransmittanceBuffer");
    auto shade_point_transmittance_ray_sampled_light_index = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    shade_point_transmittance_ray_sampled_light_index->SetName("ShadePointTransmittanceRaySampledLightIndexBuffer");

    auto shade_point_transmittance_ray_contribution = builder.CreateBuffer<glm::uvec2>(max_num_update_rays);
    shade_point_transmittance_ray_contribution->SetName("ShadePointTransmittanceRayContributionBuffer");
    auto shade_point_to_transmittance_ray_index = builder.CreateBuffer<uint32_t>(max_num_update_rays);
    shade_point_to_transmittance_ray_index->SetName("ShadePointToTransmittanceRayIndexBuffer");

    auto params = builder.Allocate<VolumeIndirectLightingParams>();
    {
        params->View = view->view_common_params_;
        params->RWVolumeProbeRadianceDepthTexture =
            view->persistent_data_->volume_indirect_lighting_persistent_data_->VolumeProbeRadianceDepthTexture.Raw();
        params->RWVolumeProbeHeaderTexture =
            view->persistent_data_->volume_indirect_lighting_persistent_data_->VolumeProbeHeaderTexture.Raw();
        params->RWVolumeProbeIrradianceTexture =
            volume_probe_irradiance.Raw();
        params->RWVolumeProbeSHCoefficientsRTexture =
            volume_probe_sh_coefficients_r.Raw();
        params->RWVolumeProbeSHCoefficientsGTexture =
            volume_probe_sh_coefficients_g.Raw();
        params->RWVolumeProbeSHCoefficientsBTexture =
            volume_probe_sh_coefficients_b.Raw();

        params->RWSpawnedVolumeProbeHeaderBuffer =
            spawned_volume_probe_header_buffer.Raw();

        params->RWVolumeProbeReconstructedRadianceDepthTexture =
            volume_probe_reconstructed_radiance_depth_texture.Raw();

        params->RWVolumeProbeSpawnAllocator = volume_probe_spawn_allocator.Raw();
        params->RWVolumeProbeMRUQueueBuffer =
            view->persistent_data_->volume_indirect_lighting_persistent_data_->VolumeProbeMRUQueueBuffer.Raw();
        params->RWVolumeProbeNextMRUQueueBuffer =
            view->volume_indirect_lighting_->volume_probe_next_mru_queue_buffer.Raw();

        params->PreviousActiveVolumeProbeCount =
            view->persistent_data_->volume_indirect_lighting_persistent_data_->ActiveVolumeProbeCount.Raw();
        params->PreviousActiveVolumeProbeListBuffer =
            view->persistent_data_->volume_indirect_lighting_persistent_data_->ActiveVolumeProbeListBuffer.Raw();

        params->RWActiveVolumeProbeCount =
            view->volume_indirect_lighting_->active_volume_probe_count.Raw();
        params->RWActiveVolumeProbeListBuffer =
            view->volume_indirect_lighting_->active_volume_probe_list_buffer.Raw();

        params->RWTileVolumeProbeIndexListBuffer =
            tile_volume_probe_index_list_buffer.Raw();
        params->RWTileVolumeProbeIndexListLengthsBuffer =
            tile_volume_probe_index_list_lengths_buffer.Raw();
        params->RWTileVolumeProbeIndexListOffsetsBuffer =
            tile_volume_probe_index_list_offsets_buffer.Raw();
        params->RWTileVolumeProbeIndexListAllocator =
            tile_volume_probe_index_list_allocator.Raw();

        params->RWTileVolumeProbeReprojectionEntryAllocator =
            tile_volume_probe_reprojection_entry_allocator.Raw();
        params->RWTileVolumeProbeReprojectionEntryBuffer =
            tile_volume_probe_reprojection_entry_buffer.Raw();
        params->RWTileSpawnedVolumeProbeBuffer =
            tile_spawned_volume_probe_header_buffer.Raw();

        params->RWVolumeProbeUpdateRayOffsetsBuffer =
            volume_probe_update_ray_offsets_buffer.Raw();
        params->RWVolumeProbeUpdateRayCountsBuffer =
            volume_probe_update_ray_counts_buffer.Raw();
        params->RWVolumeProbeUpdateRayDirectionBuffer =
            volume_probe_update_ray_direction_buffer.Raw();
        params->RWVolumeProbeUpdateRayStateBuffer =
            volume_probe_update_ray_state_buffer.Raw();
        params->RWVolumeProbeUpdateRayOriginBuffer =
            volume_probe_update_ray_origin_buffer.Raw();
        params->RWVolumeProbeUpdateRayAllocator =
            volume_probe_update_ray_allocator.Raw();

        params->RWVolumeProbeUpdateRayResultBuffer =
            volume_probe_update_ray_result_buffer.Raw();
        params->RWVolumeProbeUpdateRayRadianceBuffer =
            volume_probe_update_ray_radiance_buffer.Raw();
        params->RWVolumeProbeUpdateRayInvPdfBuffer =
            volume_probe_update_ray_inv_pdf_buffer.Raw();
        params->RWVolumeProbeUpdateRayHitResolveBucketAndCellOffsetBuffer =
            volume_probe_update_ray_hit_resolve_bucket_and_cell_offset_buffer.Raw();

        params->RWVolumeProbeUpdateRayHitShadingPointAllocator =
            volume_probe_update_ray_hit_shading_point_allocator.Raw();
        params->RWVolumeProbeUpdateRayHitShadingPointListBuffer =
            volume_probe_update_ray_hit_shading_point_list_buffer.Raw();

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

        params->PreviousNormalTexture = view->persistent_data_->g_buffer_data_->prev_G_normal_.Raw();
        params->PreviousDepthTexture = view->persistent_data_->g_buffer_data_->prev_G_depth_.Raw();
        params->PreviousShadedDiffuseRadianceWithoutEmission =
            view->persistent_data_->prev_shaded_radiance_no_emission_.Raw();

        params->G_VolumeSampleDepth = view->volume_primitives_->volume_sample_linear_depth_.Raw();
        params->G_VolumeSampleColor = view->volume_primitives_->volume_sample_color_.Raw();
        params->VolumeMinMaxTexture = view->volume_primitives_->G_volume_min_max_.Raw();
        params->VolumeDensityTexture = view->volume_primitives_->G_volume_density_.Raw();
        params->PreviousVolumeMinMaxTexture = view->persistent_data_->
            volume_primitives_view_persistent_data_->prev_volume_min_max_.Raw();
        params->PreviousVolumeRadianceTexture = view->persistent_data_->prev_shaded_volume_radiance_.Raw();
        params->PreviousVolumeDensityTexture = view->persistent_data_->
            volume_primitives_view_persistent_data_->prev_volume_density_.Raw();
        params->PreviousTransmittanceTexture = view->persistent_data_->g_buffer_data_->prev_G_transmittance_.Raw();
        params->PreviousVolumeColorTexture   = view->persistent_data_->
            volume_primitives_view_persistent_data_->prev_volume_color_.Raw();

        if (view->scene_->GetSkyTexture()) {
            params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
        } else {
            params->EnvironmentMap = nullptr;
        }
        params->RWVolumeIndirectLightingTexture =
            view->volume_indirect_lighting_->radiance.Raw();

        auto UB = builder.Allocate<VolumeIndirectLightingUB>();
        {
            UB->MaxNumUpdateRays = max_num_update_rays;
            UB->GRF_EmitterIntensityScale = CVar_GRF_EmitterIntensityScale.Get();
            UB->TileDimensions = tile_dimensions;
            UB->InvTileDimensions = 1.0f / glm::vec2(tile_dimensions);

            UB->TileCount = num_tiles;
            UB->ResetCache = need_reset;

            UB->ProbeReprojectionSearchSize = CVar_VolumeProbeSearchSize.Get();
            UB->MaxProbesToSpawnPerFrame = num_tiles;
            UB->InvProbeAtlasDimensions = 1.0f / glm::vec2(atlas_dimensions);

            UB->FrameIndex = view->persistent_data_->frame_index_;
            UB->ProbeUpdateRaysNoImportanceSampling =
                CVar_VolumeProbesRayImportanceSampling.Get() ? 0 : 1;
            UB->ScreenReuseNoDepthTesting = CVar_VolumeScreenReuseNoDepthTesting.Get() ? 1 : 0;
            UB->ProbeUpdateRaySampleSeed =
                CVar_VolumeProbesRayFreezeSeed.Get() ? 0 : (view->persistent_data_->frame_index_ + 7198272u);

            UB->ProbeUpdateRaysNoAdaptiveAllocation = 0;//CVar_AdaptiveProbeUpdateRayAllocation.Get() ? 0 : 1;
            UB->ProbeSpawnSubTileJitterSeed =
                CVar_VolumeProbesRayFreezeSeed.Get() ? 0 : view->persistent_data_->frame_index_;
            UB->TileProbeSpawnSeed =
                CVar_VolumeProbesRayFreezeSeed.Get() ? 0 : view->persistent_data_->frame_index_;
            UB->NoEnvironmentLight = CVar_NoEnvironmentLight.Get() ? 1 : 0;

            UB->NoIndirectLighting = CVar_NoIndirectLighting.Get() ? 1 : 0;
            UB->LnProbeDepthSearchTransmittanceThresh = log(CVar_VolumeProbeDepthSearchTransmittanceThreshold.Get());
            UB->NoScreenReuseEnergyDecay = CVar_NoScreenReuseEnergyDecay.Get() ? 1 : 0;
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

        // Samplers
        params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
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

    if (CVar_Debug_OutputProbePositions.Get()) {
        view->debug_buffers_.CreateVisualizeSpatialPositionsBuffers(builder, num_tiles);
        params->RWDebugVolumeProbePositionsBuffer = view->debug_buffers_.visualize_spatial_positions.Raw();
        params->RWDebugVolumeProbePositionsCount = view->debug_buffers_.visualize_spatial_positions_count.Raw();
    } else {
        params->RWDebugVolumeProbePositionsBuffer = nullptr;
        params->RWDebugVolumeProbePositionsCount = nullptr;
    }

    view->volume_indirect_lighting_->shader_params = params; // Save for further use

    {
        auto shader = lib.GetShader<ClearCountersShader>(ini);
        Helpers::AddComputePass(builder, shader, params);
    }
    // Clear per-tile lists before reuse (avoid reading garbage causing OOB)
    {
        auto shader = lib.GetShader<ClearTileVolumeProbeIndexListsShader>(ini);
        Helpers::AddComputePass<ClearTileVolumeProbeIndexListsShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }

    if (need_reset) {
        auto shader = lib.GetShader<InitializeVolumeProbeCacheShader>(ini);
        Helpers::AddComputePass<InitializeVolumeProbeCacheShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }

    {
        auto shader = lib.GetShader<InjectVolumeProbesShader>(ini);
        Helpers::AddComputePass<InjectVolumeProbesShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        auto shader = lib.GetShader<AllocateTileVolumeProbeListsShader>(ini);
        Helpers::AddComputePass<AllocateTileVolumeProbeListsShader>(
            builder, shader, params,
            DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(
            builder, tile_volume_probe_reprojection_entry_allocator.Raw(), wave_size
        );
        auto shader = lib.GetShader<ScatterReprojectedVolumeProbesToTileListShader>(ini);
        Helpers::AddComputeIndirectPass<ScatterReprojectedVolumeProbesToTileListShader>(
            builder, shader, params, cmd.Raw()
        );
    }
    {
        auto shader = lib.GetShader<SpawnVolumeProbesShader>(ini);
        Helpers::AddComputePass<SpawnVolumeProbesShader>(
            builder, shader, params,
            DivideAndRoundUp(num_tiles, wave_size)
        );
    }
    {
        auto shader = lib.GetShader<ClipVolumeProbeSpawnAllocatorShader>(ini);
        Helpers::AddComputePass<ClipVolumeProbeSpawnAllocatorShader>(builder, shader, params);
    }

    view->volume_indirect_lighting_->spawn_list_command = Helpers::SpawnDispatchIndirectCommand1D(
        builder, volume_probe_spawn_allocator.Raw(), 1
    );
    {
        auto shader = lib.GetShader<ReconstructRadiance_SampleSpawnVolumeProbeUpdateRaysShader>(ini);
        Helpers::AddComputeIndirectPass<ReconstructRadiance_SampleSpawnVolumeProbeUpdateRaysShader>(
            builder, shader, params,
            view->volume_indirect_lighting_->spawn_list_command.Raw()
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
        volume_probe_update_ray_allocator.Raw(),
        nullptr,
        volume_probe_update_ray_direction_buffer.Raw(),
        volume_probe_update_ray_state_buffer.Raw(),
        nullptr,
        volume_probe_update_ray_origin_buffer.Raw(),
        nullptr,
        volume_probe_update_ray_result_buffer.Raw(),
        view->persistent_data_->frame_index_ + 18461821u,
        VisibilityTraceType::kCoarseWithExactVolumeScattering
    );

    {
        auto shader = lib.GetShader<ResolveHitLightingFromScreenHistoryAndSpecialEmitterShader>(ini);
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(
            builder, volume_probe_update_ray_allocator.Raw(), wave_size
        );
        Helpers::AddComputeIndirectPass<ResolveHitLightingFromScreenHistoryAndSpecialEmitterShader>(
            builder, shader, params, cmd.Raw()
        );
    }

    view->volume_indirect_lighting_->shading_point_command = Helpers::SpawnDispatchIndirectCommand1D(
        builder, volume_probe_update_ray_hit_shading_point_allocator.Raw(), wave_size
    );
    {
        auto shader = lib.GetShader<SampleLightRaysForUpdateRayHitsShader>(ini);
        Helpers::AddComputeIndirectPass<SampleLightRaysForUpdateRayHitsShader>(
            builder, shader, params, view->volume_indirect_lighting_->shading_point_command.Raw()
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
            builder, volume_probe_update_ray_hit_shading_point_allocator.Raw(), wave_size
        );
        Helpers::AddComputeIndirectPass<ResolveUpdateRayHitsDirectLightingFromTraceResultShader>(
            builder, shader, params, cmd.Raw()
        );
    }
}

void Renderer::Render_FinishVolumeIndirectLighting(RendererView *view, RenderGraphBuilder &builder) {
    RDGSectionGuard section(builder, "Render_FinishVolumeIndirectLighting");
    auto & lib = RDGShaderLibrary::Get();
    auto params = view->volume_indirect_lighting_->shader_params;
    auto ini = GetVolumeIndirectLightingShaderInitializationInfo();
    {
        auto shader = lib.GetShader<ResolveProbeUpdateRayRadianceFromCellsShader>(ini);
        Helpers::AddComputeIndirectPass<ResolveProbeUpdateRayRadianceFromCellsShader>(
            builder, shader, params, view->volume_indirect_lighting_->shading_point_command.Raw()
        );
    }

    {
        auto ini_s = ini;
        if (CVar_Debug_OutputProbeUpdateRays.Get()) ini_s.optional_macros.push_back("DEBUG_OUTPUT_TRACED_RAY");
        auto shader = lib.GetShader<UpdateVolumeProbesAndCacheShader>(ini_s);
        Helpers::AddComputeIndirectPass<UpdateVolumeProbesAndCacheShader>(
            builder, shader, params,
            view->volume_indirect_lighting_->spawn_list_command.Raw()
        );
    }

    auto tile_dimensions = GetTileDimensions(view);
    {
        auto num_tiles = tile_dimensions.x * tile_dimensions.y;
        auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
        auto shader = lib.GetShader<UpdateVolumeProbeCacheMRUQueueShader>(ini);
        Helpers::AddComputePass<UpdateVolumeProbeCacheMRUQueueShader>(
            builder, shader, params, DivideAndRoundUp(num_tiles, wave_size)
        );
    }

    {
        auto shader = lib.GetShader<ComputeVolumeProbeSHCoefficientsShader>(ini);
        Helpers::AddComputePass<ComputeVolumeProbeSHCoefficientsShader>(
            builder, shader, params,
            tile_dimensions.x, tile_dimensions.y
        );
    }

    {
        auto shader = lib.GetShader<ComputeVolumeIndirectLightingShader>(ini);
        Helpers::AddComputePass<ComputeVolumeIndirectLightingShader>(
            builder, shader, params,
            tile_dimensions.x,
            tile_dimensions.y
        );
    }

    if (CVar_Debug_OutputProbePositions.Get()) {
        auto shader = lib.GetShader<DebugOutputVolumeProbePositionsShader>(ini);
        auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(
            builder, view->volume_indirect_lighting_->active_volume_probe_count.Raw(), wave_size
        );
        Helpers::AddComputeIndirectPass<DebugOutputVolumeProbePositionsShader>(
            builder, shader, params, cmd.Raw()
        );
    }
}

MI_NAMESPACE_END
