/*
 * Created: 2025/10/6
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

struct HashGridWorldCacheUB {
    uint32_t MaxNumTiles;
    uint32_t FrameIndex;
    uint32_t TileLifeSpan;
    uint32_t MaxNumSamples;
    glm::vec3 Center;
    float InvCascadeRadius;

    float CellSize;
    uint32_t  NumBuckets;
    uint32_t  MaxNumEntriesSearchedPerBucket;
    uint32_t  NumInterleavedEntriesPerBucket;

    uint32_t  TargetSampleCount;
    glm::uvec3 Padding;
};

BEGIN_SHADER_PARAMETERS(HashGridCommonParameters)
    // Tile allocator
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_FreeTileCountBuffer)
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
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_UpdateTileCountBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_UpdateTileListBuffer)
    // A list of active tile indices
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileCountBeforeAllocationBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileCountBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_HistoryActiveTileCountBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_HistoryActiveTileListBuffer)

    SHADER_UNIFORM_BUFFER(HashGridWorldCacheUB, HashGrids_UB)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(HashGridCommonParameters)

class ResetHashGridsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ResetHashGridsShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "ResetHashGrids")

class ReInsertHashGridTilesShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ReInsertHashGridTilesShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "ReInsertHashGridTiles")

class PrepareDispatchCommandForClearNewHashGridTileCellsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(PrepareDispatchCommandForClearNewHashGridTileCellsShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "PrepareDispatchCommandForClearNewHashGridTileCells")

class ClearNewHashGridTileCellsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearNewHashGridTileCellsShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "ClearNewHashGridTileCells")

class FilterHashGridsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(FilterHashGridsShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "FilterHashGrids")

void Renderer::Render_UpdateHashGridCache(RendererView *view, RenderGraphBuilder &builder) {
    auto params = builder.Allocate<HashGridCommonParameters>();
    {
        auto persistent = view->persistent_data_->hash_grid_world_cache_;
        params->HashGrids_FreeTileCountBuffer =
    }
    auto & lib = RDGShaderLibrary::Get();
}


MI_NAMESPACE_END