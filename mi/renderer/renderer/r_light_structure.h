/*
 * Created: 2025/10/10
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_LIGHT_STRUCTURE_H
#define MI_R_LIGHT_STRUCTURE_H
#include <renderer/mi_renderer_view.h>

#include "r_persistent.h"

MI_NAMESPACE_BEGIN

extern CVar<int> CVar_MaxNumGridLights;
extern CVar<int> CVar_NumLightSamplerSamples;
extern CVar<int> CVar_MaxNumLightGridEntries;

static constexpr uint32_t kLightGridSize = 16;
static constexpr uint32_t kLightGridNumCascades = 6; // Number of cascades in the light grid
static constexpr uint32_t kLightGridNumHistories = 4; // Number of history frames for visibility caching

// Must be consistent with the struct in LightGrid.hlsl
struct LightStructureUB {
    glm::uvec3 LightGridSize;
    float LightGridCellSize;
    glm::vec3 LightGridCenter;
    uint32_t LighGridNumCascadesUsed;
    uint32_t LightGridMaxNumGridLights;
    uint32_t LightGridNumCascadeGrids;
    uint32_t LightGridNumGrids;
    float LightInjectionIntensityThreshold;
    glm::vec4 LightGridCascadeMin[kLightGridNumCascades];
    glm::vec4 LightGridCascadeMax[kLightGridNumCascades];
    uint32_t FrameIndex;
    uint32_t MaxNumLights;
    float    EnvironmentLightHemisphereSampleLOD;
    uint32_t Unused;
};

struct LightStructurePersistentData : RefCounted<> {
    TRef<RDGBuffer> environment_visibility_history_buffer;
    TRef<RDGBuffer> bloom_filter_buffer;

    // If the buffers need to be reset to initial state (possibly upon first start or buffer
    // reallocation)
    bool need_reset_ {true};

    bool MakeSureExists(
        RendererView * view, RenderGraphBuilder & builder
    );

    // Called at the end of the frame to update persistent data
    void FinalUpdate (
        RendererView * view
    );
};

struct LightStructureData : RefCounted<> {
    TRef<RDGBuffer> precomputed_active_light_buffer;
    TRef<RDGBuffer> active_light_list_count;
    TRef<RDGBuffer> active_light_list_buffer;
    // List of light indices for each grid
    TRef<RDGBuffer> list_allocator;
    TRef<RDGBuffer> list_active_light_list_index_buffer; // stores a index to active light list
    TRef<RDGBuffer> grid_light_list_offset_buffer;
    TRef<RDGBuffer> grid_light_list_cdf_buffer;
    TRef<RDGBuffer> grid_light_list_length_buffer;

    TRef<RDGBuffer> next_bloom_filter_buffer;
    TRef<RDGBuffer> next_environment_visibility_buffer;

    void Allocate(
        RenderGraphBuilder & builder
    );
};

template<typename T>
void FillParametersForLightStructure (RendererView * view, T * params) {
    auto ls = view->light_structure_;

    if constexpr(requires{params->LightGrid_PrecomputedActiveLightBuffer;}) {
        params->LightGrid_PrecomputedActiveLightBuffer = ls->precomputed_active_light_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_ActiveLightListCount;}) {
        params->LightGrid_ActiveLightListCount = ls->active_light_list_count.Raw();
    }
    if constexpr(requires{params->LightGrid_ActiveLightListBuffer;}) {
        params->LightGrid_ActiveLightListBuffer = ls->active_light_list_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_ListAllocator;}) {
        params->LightGrid_ListAllocator = ls->list_allocator.Raw();
    }
    if constexpr(requires{params->LightGrid_ListActiveLightListIndexBuffer;}) {
        params->LightGrid_ListActiveLightListIndexBuffer = ls->list_active_light_list_index_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_GridLightListOffsetBuffer;}) {
        params->LightGrid_GridLightListOffsetBuffer = ls->grid_light_list_offset_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_GridLightListCdfBuffer;}) {
        params->LightGrid_GridLightListCdfBuffer = ls->grid_light_list_cdf_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_GridLightListLengthBuffer;}) {
        params->LightGrid_GridLightListLengthBuffer = ls->grid_light_list_length_buffer.Raw();
    }
    auto persistent = view->persistent_data_->light_structure_persistent_data_;
    if constexpr(requires{params->LightGrid_EnvironmentVisibilityHistoryBuffer;}) {
        params->LightGrid_EnvironmentVisibilityHistoryBuffer = persistent->environment_visibility_history_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_BloomFilterBuffer;}) {
        params->LightGrid_BloomFilterBuffer = persistent->bloom_filter_buffer.Raw();
    }

    if constexpr(requires{params->LightGrid_NextBloomFilterBuffer;}) {
        params->LightGrid_NextBloomFilterBuffer = ls->next_bloom_filter_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_NextEnvironmentVisibilityBuffer;}) {
        params->LightGrid_NextEnvironmentVisibilityBuffer = ls->next_environment_visibility_buffer.Raw();
    }
}

void FillUniformBufferForLightStructure (RendererView * view, LightStructureUB * UB) ;

std::vector<std::string> GetLightStructureShaderMacros () ;

MI_NAMESPACE_END

#endif //MI_R_LIGHT_STRUCTURE_H