/*
 * Created: 2025/10/10
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_LIGHT_STRUCTURE_H
#define MI_R_LIGHT_STRUCTURE_H
#include <renderer/mi_renderer_view.h>
MI_NAMESPACE_BEGIN

static constexpr uint32_t kLightGridSize = 16;
static constexpr uint32_t kLightGridNumCascades = 6; // Number of cascades in the light grid

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
    glm::uvec2 Unused;
};

struct LightStructureData : RefCounted<> {
    TRef<RDGBuffer> active_light_list_buffer;
    TRef<RDGBuffer> precomputed_active_light_buffer;
    TRef<RDGBuffer> list_light_index_buffer;
    TRef<RDGBuffer> grid_light_list_offset_buffer;
    TRef<RDGBuffer> grid_light_list_cdf_buffer;
    TRef<RDGBuffer> grid_light_list_length_buffer;

    void Allocate(
        RenderGraphBuilder & builder
    );
};

template<typename T>
void FillParametersForLightStructure (RendererView * view, T * params) {
    auto ls = view->light_structure_;
    if constexpr(requires{params->LightStructure_ActiveLightListBuffer;}) {
        params->LightStructure_ActiveLightListBuffer = ls->active_light_list_buffer.Raw();
    }
    if constexpr(requires{params->LightStructure_PrecomputedActiveLightBuffer;}) {
        params->LightStructure_PrecomputedActiveLightBuffer = ls->precomputed_active_light_buffer.Raw();
    }
    if constexpr(requires{params->LightStructure_ListLightIndexBuffer;}) {
        params->LightStructure_ListLightIndexBuffer = ls->list_light_index_buffer.Raw();
    }
    if constexpr(requires{params->LightStructure_GridLightListOffsetBuffer;}) {
        params->LightStructure_GridLightListOffsetBuffer = ls->grid_light_list_offset_buffer.Raw();
    }
    if constexpr(requires{params->LightStructure_GridLightListCDFBuffer;}) {
        params->LightStructure_GridLightListCDFBuffer = ls->grid_light_list_cdf_buffer.Raw();
    }
    if constexpr(requires{params->LightStructure_GridLightListLengthBuffer;}) {
        params->LightStructure_GridLightListLengthBuffer = ls->grid_light_list_length_buffer.Raw();
    }
}

void FillUniformBufferForLightStructure (RendererView * view, LightStructureUB * UB) ;

MI_NAMESPACE_END

#endif //MI_R_LIGHT_STRUCTURE_H