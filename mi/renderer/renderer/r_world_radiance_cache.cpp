/*
 * Created: 2025/10/6
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <rdg/rdg.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_renderer.h>
#include "r_view_common.h"
#include "r_world_radiance_cache.h"
#include "r_persistent.h"
#include "../shaders/shared/SharedHashGridCache.hlsl"

MI_NAMESPACE_BEGIN

bool HashGridPersistentData::MakeSureExists(
    [[maybe_unused]] RendererView *view, RenderGraphBuilder &builder,
    uint32_t max_num_tiles, [[maybe_unused]] uint32_t num_buckets, [[maybe_unused]] uint32_t num_elements_per_bucket) {
    bool flag = false;
    if (!free_tile_count) {
        free_tile_count = builder.CreateBuffer<uint32_t>();
        free_tile_count->SetExport();
        flag = true;
    }
    if (!free_tile_list_buffer) {
        free_tile_list_buffer = builder.CreateBuffer<uint32_t>(max_num_tiles);
        free_tile_list_buffer->SetExport();
        flag = true;
    }
    if (!tile_timestamp_buffer) {
        tile_timestamp_buffer = builder.CreateBuffer<uint32_t>(max_num_tiles);
        tile_timestamp_buffer->SetExport();
        flag = true;
    }
    if (!tile_bucket_hash_buffer) {
        tile_bucket_hash_buffer = builder.CreateBuffer<uint32_t>(max_num_tiles);
        tile_bucket_hash_buffer->SetExport();
        flag = true;
    }
    // Fp16x4 packed
    if (!cell_value_buffer) {
        cell_value_buffer = builder.CreateBuffer<glm::uvec2>(max_num_tiles * HASHGRIDS_NUM_CELLS_PER_TILE);
        cell_value_buffer->SetExport();
        flag = true;
    }
    if (!update_cell_value_x_buffer) {
        update_cell_value_x_buffer = builder.CreateBuffer<uint32_t>(
            // mip 0 only, 4 atomic integers per cell
            max_num_tiles * HASHGRIDS_TILE_CELL_MIP_OFFSET_1 * 4
        );
        update_cell_value_x_buffer->SetExport();
        flag = true;
    }

    if (!active_tile_count) {
        active_tile_count = builder.CreateBuffer<uint32_t>();
        active_tile_count->SetExport();
        flag = true;
    }
    if (!active_tile_list_buffer) {
        active_tile_list_buffer = builder.CreateBuffer<uint32_t>(max_num_tiles);
        active_tile_list_buffer->SetExport();
        flag = true;
    }
    return flag;
}

void RendererView::MakeSureHashGridPersistentDataExists(RenderGraphBuilder &builder) {
    const uint32_t max_num_tiles = kHashGridMaxNumTiles;
    const uint32_t num_buckets = kHashGridMaxNumBuckets;
    const uint32_t num_elements_per_bucket = kHashGridNumElementsPerBucket;
    if (!persistent_data_->hash_grid_persistent_data_) {
        persistent_data_->hash_grid_persistent_data_ = new HashGridPersistentData();
    }
    auto persistent = persistent_data_->hash_grid_persistent_data_;
    persistent->MakeSureExists(this, builder, max_num_tiles, num_buckets, num_elements_per_bucket);
}


void WorldRadianceCacheData::Allocate(RenderGraphBuilder & builder) {
    // TODO CVar system does not support dirty tracking currently.
    // So we have to make these parameters constant.
    const uint32_t max_num_tiles = kHashGridMaxNumTiles;
    const uint32_t num_buckets = kHashGridMaxNumBuckets;
    const uint32_t num_elements_per_bucket = kHashGridNumElementsPerBucket;
    // const float cascade_radius = kHashGridCascadeRadius;
    // const float cell_size = kHashGridCellSize;

    bucket_hash_buffer = builder.CreateBuffer<uint32_t>(num_buckets * num_elements_per_bucket);
    bucket_tile_index_buffer = builder.CreateBuffer<uint32_t>(num_buckets * num_elements_per_bucket);
    update_tile_count_buffer = builder.CreateBuffer<uint32_t>();
    update_tile_list_buffer = builder.CreateBuffer<uint32_t>(max_num_tiles);
    active_tile_count_before_allocation_buffer = builder.CreateBuffer<uint32_t>();
    active_tile_count = builder.CreateBuffer<uint32_t>();
    active_tile_list_buffer = builder.CreateBuffer<uint32_t>(max_num_tiles);
}


IMPLEMENT_SHADER_PARAMETERS(HashGridCommonParameters)

class ResetHashGridsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size)
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ResetHashGridsShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "ResetHashGrids")

class ReInsertHashGridTilesShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size)
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ReInsertHashGridTilesShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "ReInsertHashGridTiles")

class PrepareDispatchCommandForClearNewHashGridTileCellsShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_FreeTileCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, HashGrids_ActiveTileCountBeforeAllocationBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWClearNewHashGridTileCellsIndirectCommandBuffer)
    END_SHADER_PARAMETERS()
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(Params)
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size)
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(PrepareDispatchCommandForClearNewHashGridTileCellsShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "PrepareDispatchCommandForClearNewHashGridTileCells")

class ClearNewHashGridTileCellsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size)
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearNewHashGridTileCellsShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "ClearNewHashGridTileCells")

class FilterHashGridsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(HashGridCommonParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size)
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(FilterHashGridsShader, "mi/renderer/shaders/HashGridWorldCache.hlsl", "FilterHashGrids")


void Renderer::Render_ReuseHashGridCache(RendererView *view, RenderGraphBuilder &builder) {
    // Initialize and reuse the hash grid cache from the previous frame.
    auto params = builder.Allocate<HashGridCommonParameters>();

    const uint32_t max_num_tiles = kHashGridMaxNumTiles;
    // const uint32_t num_buckets = kHashGridMaxNumBuckets;
    // const uint32_t num_elements_per_bucket = kHashGridNumElementsPerBucket;

    bool need_reset = false;

    // Make sure to reset the cache when persistent data is created.
    auto persistent = view->persistent_data_->hash_grid_persistent_data_;
    need_reset |= persistent->need_reset_;
    persistent->need_reset_ = false;
    {
        FillParametersForHashGridCache(view, params);
        auto UB = builder.Allocate<HashGridWorldCacheUB>();
        FillUniformBufferForHashGridCache(view, UB);
        params->HashGrids_UB = UB;
    }
    auto & lib = RDGShaderLibrary::Get();
    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
    if (need_reset) {
        auto shader = lib.GetShader<ResetHashGridsShader>();
        Helpers::AddComputePass(builder, shader, params, DivideAndRoundUp(max_num_tiles, wave_size));
        Helpers::Clear(builder, view->persistent_data_->hash_grid_persistent_data_->update_cell_value_x_buffer.Raw());
    }

    {
        Helpers::Clear(builder, view->world_cache_->bucket_hash_buffer.Raw());
        auto shader = lib.GetShader<ReInsertHashGridTilesShader>();
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, persistent->active_tile_count.Raw(), wave_size);
        Helpers::AddComputeIndirectPass(builder, shader, params, cmd.Raw());
    }

    auto w = view->world_cache_;
    Helpers::CopyBuffer(builder, w->active_tile_count.Raw(), w->active_tile_count_before_allocation_buffer.Raw());
}

void Renderer::Render_UpdateHashGridCache(RendererView *view, RenderGraphBuilder &builder) {
    // Clear newly allocated tiles and filter hash grids this frame.
    auto params = builder.Allocate<HashGridCommonParameters>();

    // const uint32_t max_num_tiles = kHashGridMaxNumTiles;
    // const uint32_t num_buckets = kHashGridMaxNumBuckets;
    // const uint32_t num_elements_per_bucket = kHashGridNumElementsPerBucket;

    auto clear_cmd = builder.CreateBuffer<RHIDispatchIndirectCommand>(
        RHIBufferUsageFlagBits::kIndirect | RHIBufferUsageFlagBits::kStorage
    );
    {
        FillParametersForHashGridCache(view, params);
        auto UB = builder.Allocate<HashGridWorldCacheUB>();
        FillUniformBufferForHashGridCache(view, UB);
        params->HashGrids_UB = UB;
    }
    auto & lib = RDGShaderLibrary::Get();
    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
    auto persistent = view->persistent_data_->hash_grid_persistent_data_;
    {
        auto shader = lib.GetShader<PrepareDispatchCommandForClearNewHashGridTileCellsShader>();
        auto prepare_params = builder.Allocate<PrepareDispatchCommandForClearNewHashGridTileCellsShader::Params>();
        {
            prepare_params->HashGrids_FreeTileCount = persistent->free_tile_count.Raw();
            prepare_params->HashGrids_ActiveTileCount = view->world_cache_->active_tile_count.Raw();
            prepare_params->HashGrids_ActiveTileCountBeforeAllocationBuffer = view->world_cache_->active_tile_count_before_allocation_buffer.Raw();
            prepare_params->RWClearNewHashGridTileCellsIndirectCommandBuffer = clear_cmd.Raw();
        }
        Helpers::AddComputePass(builder, shader, prepare_params, 1);
    }
    {
        auto shader = lib.GetShader<ClearNewHashGridTileCellsShader>();
        Helpers::AddComputeIndirectPass(builder, shader, params, clear_cmd.Raw());
    }
    auto w = view->world_cache_;
    {
        auto shader = lib.GetShader<FilterHashGridsShader>();
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, w->active_tile_count.Raw());
        Helpers::AddComputeIndirectPass(builder, shader, params, cmd.Raw());
    }
    // Update persistent data
    {
        persistent->active_tile_count = w->active_tile_count;
        persistent->active_tile_count->SetExport();
        persistent->active_tile_list_buffer = w->active_tile_list_buffer;
        persistent->active_tile_list_buffer->SetExport();
    }
}


MI_NAMESPACE_END