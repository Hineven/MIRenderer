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
extern CVar<float> CVar_LightCullingRate;
extern CVar<glm::vec3> CVar_EnvironmentLightMultiplier;
extern CVar<float> CVar_EnvironmentLightEvaluateLOD;

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
    uint32_t LightGridMaxNumEntries;
    float    EnvironmentLightHemisphereSampleLOD;

    float    LightCullingRate;
    glm::vec3   EnvironmentLightMultiplier;

    float EnvironmentLightEvaluateLOD;
    uint32_t NumMLIClusters;
    uint32_t MaxNumActiveLightGrids;
    uint32_t padding;
};

struct LightStructurePersistentData : RefCounted<> {
    TRef<RDGBuffer> environment_visibility_history_buffer;
    TRef<RDGBuffer> bloom_filter_buffer;
    TRef<RDGBuffer> active_grid_flag_buffer;
    TRef<RDGBuffer> grid_pressure_buffer;

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
    TRef<RDGBuffer> active_grid_count;
    TRef<RDGBuffer> active_grid_indices_buffer;
    TRef<RDGBuffer> active_mesh_light_instance_count;
    TRef<RDGBuffer> active_mesh_light_instance_index_buffer;
    TRef<RDGBuffer> precompute_triangle_draw_command_buffer;
    TRef<RDGBuffer> precompute_finalize_cluster_draw_command_buffer;
    std::vector<TRef<RDGBuffer>> precompute_level_draw_command_buffers;
    uint32_t num_active_mesh_light_instances_ {};
    uint32_t num_active_mesh_light_instance_clusters_ {};
    uint32_t max_active_mesh_light_instance_levels_ {};
    TRef<RDGBuffer> active_light_list_count;
    TRef<RDGBuffer> active_light_list_buffer;
    // List of light indices for each grid
    TRef<RDGBuffer> list_allocator;
    TRef<RDGBuffer> list_active_light_list_index_buffer; // stores a index to active light list
    TRef<RDGBuffer> list_mesh_light_instance_element_index_buffer;
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
    auto persistent = view->persistent_data_->light_structure_persistent_data_;

    if constexpr(requires{params->LightGrid_RWActiveGridAllocator;}) {
        params->LightGrid_RWActiveGridAllocator = ls->active_grid_count.Raw();
    }
    if constexpr(requires{params->LightGrid_RWActiveGridIndicesBuffer;}) {
        params->LightGrid_RWActiveGridIndicesBuffer = ls->active_grid_indices_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWActiveGridFlagBuffer;}) {
        params->LightGrid_RWActiveGridFlagBuffer = persistent->active_grid_flag_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWGridPressureBuffer;}) {
        params->LightGrid_RWGridPressureBuffer = persistent->grid_pressure_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWActiveMeshLightInstanceCount;}) {
        params->LightGrid_RWActiveMeshLightInstanceCount = ls->active_mesh_light_instance_count.Raw();
    }
    if constexpr(requires{params->LightGrid_ActiveMeshLightInstanceIndexBuffer;}) {
        params->LightGrid_ActiveMeshLightInstanceIndexBuffer = ls->active_mesh_light_instance_index_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWActiveLightListCount;}) {
        params->LightGrid_RWActiveLightListCount = ls->active_light_list_count.Raw();
    }
    if constexpr(requires{params->LightGrid_RWActiveLightListBuffer;}) {
        params->LightGrid_RWActiveLightListBuffer = ls->active_light_list_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWListAllocator;}) {
        params->LightGrid_RWListAllocator = ls->list_allocator.Raw();
    }
    if constexpr(requires{params->LightGrid_RWListActiveLightListIndexBuffer;}) {
        params->LightGrid_RWListActiveLightListIndexBuffer = ls->list_active_light_list_index_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWListMeshLightInstanceElementIndexBuffer;}) {
        params->LightGrid_RWListMeshLightInstanceElementIndexBuffer = ls->list_mesh_light_instance_element_index_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWGridLightListOffsetBuffer;}) {
        params->LightGrid_RWGridLightListOffsetBuffer = ls->grid_light_list_offset_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWGridLightListCdfBuffer;}) {
        params->LightGrid_RWGridLightListCdfBuffer = ls->grid_light_list_cdf_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWGridLightListLengthBuffer;}) {
        params->LightGrid_RWGridLightListLengthBuffer = ls->grid_light_list_length_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWEnvironmentVisibilityHistoryBuffer;}) {
        params->LightGrid_RWEnvironmentVisibilityHistoryBuffer = persistent->environment_visibility_history_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWBloomFilterBuffer;}) {
        params->LightGrid_RWBloomFilterBuffer = persistent->bloom_filter_buffer.Raw();
    }

    if constexpr(requires{params->LightGrid_RWNextBloomFilterBuffer;}) {
        params->LightGrid_RWNextBloomFilterBuffer = ls->next_bloom_filter_buffer.Raw();
    }
    if constexpr(requires{params->LightGrid_RWNextEnvironmentVisibilityBuffer;}) {
        params->LightGrid_RWNextEnvironmentVisibilityBuffer = ls->next_environment_visibility_buffer.Raw();
    }
}

void FillUniformBufferForLightStructure (RendererView * view, LightStructureUB * UB) ;

std::vector<std::string> GetLightStructureShaderMacros () ;

MI_NAMESPACE_END

#endif //MI_R_LIGHT_STRUCTURE_H
