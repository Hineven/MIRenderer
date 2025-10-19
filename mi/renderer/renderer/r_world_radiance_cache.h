/*
 * Created: 2025/10/9
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_WORLD_RADIANCE_CACHE_H
#define MI_R_WORLD_RADIANCE_CACHE_H
#include <renderer/mi_cvar.h>
#include <rdg/rdg_resource.h>
#include "r_persistent.h"
#include "../shaders/shared/SharedHashGridCache.hlsl"

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

struct HashGridPersistentData : RefCounted<> {
    TRef<RDGBuffer> free_tile_count;
    TRef<RDGBuffer> free_tile_list_buffer;
    TRef<RDGBuffer> tile_timestamp_buffer;
    TRef<RDGBuffer> tile_bucket_hash_buffer;
    TRef<RDGBuffer> cell_value_buffer;
    TRef<RDGBuffer> update_cell_value_x_buffer;

    TRef<RDGBuffer> active_tile_count;
    TRef<RDGBuffer> active_tile_list_buffer;

    bool need_reset_ {true};

    bool MakeSureExists(
        RendererView * view, RenderGraphBuilder & builder,
        uint32_t max_num_tiles,
        uint32_t num_buckets, uint32_t num_elements_per_bucket
    ) ;
};

struct WorldRadianceCacheData : RefCounted<> {
    TRef<RDGBuffer> bucket_hash_buffer;
    TRef<RDGBuffer> bucket_tile_index_buffer;
    TRef<RDGBuffer> update_tile_count_buffer;
    TRef<RDGBuffer> update_tile_list_buffer;
    TRef<RDGBuffer> active_tile_count_before_allocation_buffer;
    TRef<RDGBuffer> active_tile_count;
    TRef<RDGBuffer> active_tile_list_buffer;

    void Allocate(
        RenderGraphBuilder & builder
    );
};
constexpr static uint32_t kHashGridMaxNumTiles = 64 * 1024;
constexpr static uint32_t kHashGridMaxNumBuckets = 64 * 1024;
constexpr static uint32_t kHashGridNumElementsPerBucket = 2;

static_assert(kHashGridMaxNumBuckets * kHashGridNumElementsPerBucket  < 1 << std::max(32 - 2 * HASHGRIDS_TILE_CELL_WIDTH_L2, 0),
    "Too many bucket slots. Packing the bucket slot index & tile cell offset in a 32-bit integer won't work."
    "(Which is hard-coded in the shaders PackBucketSlotAndCellOffset)."
);

// TODO make the following two configurable
constexpr static float kHashGridCascadeRadius = 6.4f;
constexpr static float kHashGridCellSize = 0.1f;

template<typename T>
void FillParametersForHashGridCache (RendererView * view, T * params) {
    auto persistent = view->persistent_data_->hash_grid_persistent_data_;
    {
        if constexpr(requires{params->HashGrids_FreeTileCount;}) {
            params->HashGrids_FreeTileCount       = persistent->free_tile_count.Raw();
        }
        if constexpr(requires{params->HashGrids_FreeTileListBuffer;}) {
            params->HashGrids_FreeTileListBuffer  = persistent->free_tile_list_buffer.Raw();
        }
        auto w = view->world_cache_;
        if constexpr(requires{params->HashGrids_BucketHashBuffer;}) {
            params->HashGrids_BucketHashBuffer = w->bucket_hash_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_BucketTileIndexBuffer;}) {
            params->HashGrids_BucketTileIndexBuffer = w->bucket_tile_index_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_TileTimestampBuffer;}) {
            params->HashGrids_TileTimestampBuffer = persistent->tile_timestamp_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_TileBucketHashBuffer;}) {
            params->HashGrids_TileBucketHashBuffer = persistent->tile_bucket_hash_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_CellValueBuffer;}) {
            params->HashGrids_CellValueBuffer = persistent->cell_value_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_UpdateCellValueXBuffer;}) {
            params->HashGrids_UpdateCellValueXBuffer = persistent->update_cell_value_x_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_UpdateTileCount;}) {
            params->HashGrids_UpdateTileCount      = w->update_tile_count_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_UpdateTileListBuffer;}) {
            params->HashGrids_UpdateTileListBuffer = w->update_tile_list_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_ActiveTileCountBeforeAllocationBuffer;}) {
            params->HashGrids_ActiveTileCountBeforeAllocationBuffer = w->active_tile_count_before_allocation_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_ActiveTileCount;}) {
            params->HashGrids_ActiveTileCount = w->active_tile_count.Raw();
        }
        if constexpr(requires{params->HashGrids_ActiveTileListBuffer;}) {
            params->HashGrids_ActiveTileListBuffer = w->active_tile_list_buffer.Raw();
        }
        if constexpr(requires{params->HashGrids_HistoryActiveTileCount;}) {
            params->HashGrids_HistoryActiveTileCount = persistent->active_tile_count.Raw();
        }
        if constexpr(requires{params->HashGrids_HistoryActiveTileListBuffer;}) {
            params->HashGrids_HistoryActiveTileListBuffer = persistent->active_tile_list_buffer.Raw();
        }
    }
}

FORCEINLINE void FillUniformBufferForHashGridCache (RendererView * view, HashGridWorldCacheUB * UB) {
    const uint32_t max_num_tiles = kHashGridMaxNumTiles;
    const uint32_t num_buckets = kHashGridMaxNumBuckets;
    const uint32_t num_elements_per_bucket = kHashGridNumElementsPerBucket;
    const float cascade_radius = kHashGridCascadeRadius;
    const float cell_size = kHashGridCellSize;
    UB->MaxNumTiles = max_num_tiles;
    UB->FrameIndex = view->persistent_data_->frame_index_;
    UB->TileLifeSpan = 30;
    UB->MaxNumSamples = 128;
    UB->Center = view->camera_.position;
    UB->InvCascadeRadius = 1.f / cascade_radius;

    UB->CellSize = cell_size;
    UB->NumBuckets = num_buckets;
    mi_check(num_elements_per_bucket <= HASHGRIDS_MAX_NUM_ENTRIES_SEARCHED_PER_BUCKET, "Too many elements per bucket.");
    UB->MaxNumEntriesSearchedPerBucket = std::min(uint32_t(num_elements_per_bucket * 4), (uint32_t)HASHGRIDS_MAX_NUM_ENTRIES_SEARCHED_PER_BUCKET);
    UB->NumInterleavedEntriesPerBucket = num_elements_per_bucket;

    UB->TargetSampleCount = 64;
}

MI_NAMESPACE_END
#endif //MI_R_WORLD_RADIANCE_CACHE_H