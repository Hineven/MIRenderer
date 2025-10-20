/*
 * Created: 2025/10/10
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "r_light_structure.h"

#include "r_persistent.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "../shaders/shared/SharedLight.hlsl"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
MI_NAMESPACE_BEGIN
CVar<int> CVar_MaxNumGridLights(
    "r.lightgrid.max_num_grid_lights",
    "Maximum number of lights in each grid cell.",
    32
);
CVar<int> CVar_MaxNumLightGridEntries(
    "r.lightgrid.max_num_entries",
    "Maximum number of entries in the light grid.",
    1024 * 1024
);
static CVar<float> CVar_MinLightGridSize(
    "r.lightgrid.min_size",
    "Minimum size of the light grid in world units.",
    0.15f
);
static CVar<float> CVar_LightInjectionIntensityThreshold(
    "r.lightgrid.light_injection_intensity_threshold",
    "Threshold for light injection intensity. Lights with intensity below this value will not be injected into the grid.",
    0.001f
);

CVar<int> CVar_NumLightSamplerSamples(
    "r.lightgrid.num_light_sampler_samples",
    "Number of candidate samples to take when sampling lights in the light grid.",
    8
);

static CVar<bool> CVar_DebugFreezeFrameSeed(
    "r.lightgrid.debug.freeze_frame_seed",
    "Freeze the frame index used for random seed generation when injecting lights. This is useful for debugging.",
    false
);

void LightStructureData::Allocate(RenderGraphBuilder &builder) {
    auto & r = Renderer::Get();
    auto max_num_lights = r.GetDeviceAllocator()->GetAreaLightsUberBuffer()->GetAllocationLimitByteOffset() / sizeof(RawLight);
    auto num_light_grids = kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize;

    precomputed_active_light_buffer = builder.CreateBuffer<PackedPrecomputedLight>(max_num_lights);
    precomputed_active_light_buffer->SetName("LightGrid_PrecomputedActiveLightBuffer");
    active_light_list_count = builder.CreateBuffer<uint32_t>();
    active_light_list_count->SetName("LightGrid_ActiveLightListCount");
    active_light_list_buffer = builder.CreateBuffer<uint32_t>(max_num_lights);
    active_light_list_buffer->SetName("LightGrid_ActiveLightListBuffer");
    auto max_num_light_grid_entries = CVar_MaxNumLightGridEntries.Get();
    list_active_light_list_index_buffer = builder.CreateBuffer<uint32_t>(max_num_light_grid_entries);
    list_allocator = builder.CreateBuffer<uint32_t>();
    list_allocator->SetName("LightGrid_ListAllocator");
    list_active_light_list_index_buffer->SetName("LightGrid_ListActiveLightListIndexBuffer");
    grid_light_list_offset_buffer = builder.CreateBuffer<uint32_t>(num_light_grids);
    grid_light_list_offset_buffer->SetName("LightGrid_GridLightListOffsetBuffer");
    grid_light_list_cdf_buffer = builder.CreateBuffer<float>(num_light_grids);
    grid_light_list_cdf_buffer->SetName("LightGrid_GridLightListCdfBuffer");
    grid_light_list_length_buffer = builder.CreateBuffer<uint32_t>(num_light_grids);
    grid_light_list_length_buffer->SetName("LightGrid_GridLightListLengthBuffer");

    environment_visibility_history_buffer = builder.CreateBuffer<uint32_t>(max_num_light_grid_entries * kLightGridNumHistories);
    environment_visibility_history_buffer->SetName("LightGrid_EnvironmentVisibilityHistoryBuffer");
    bloom_filter_buffer = builder.CreateBuffer<glm::uvec2>(max_num_light_grid_entries * kLightGridNumHistories);
    bloom_filter_buffer->SetName("LightGrid_BloomFilterBuffer");
}


void FillUniformBufferForLightStructure(RendererView *view, LightStructureUB *UB) {
    auto max_num_lights = Renderer::Get().GetDeviceAllocator()->GetAreaLightsUberBuffer()
        ->GetAllocationLimitByteOffset() / sizeof(RawLight);
    auto scene_aabb = view->scene_->GetAABB();
    auto camera_pos = view->camera_.position;
    auto dist_to_min = scene_aabb.min - camera_pos;
    auto dist_to_max = scene_aabb.max - camera_pos;
    auto maxv = glm::max(glm::abs(dist_to_min), glm::abs(dist_to_max));
    auto hmaxv = std::max(maxv.x, std::max(maxv.y, maxv.z));
    float grid_cell_size = 4.01f * float(double(hmaxv) / pow(2, kLightGridNumCascades) / kLightGridSize);
    grid_cell_size = std::max(grid_cell_size, CVar_MinLightGridSize.Get());
    UB->LightGridSize = glm::uvec3(kLightGridSize);
    UB->LightGridCellSize = grid_cell_size;
    UB->LightGridCenter = camera_pos;
    UB->LighGridNumCascadesUsed = kLightGridNumCascades;
    UB->LightGridMaxNumGridLights = CVar_MaxNumGridLights.Get();
    UB->LightGridNumCascadeGrids = kLightGridSize * kLightGridSize * kLightGridSize;
    [[maybe_unused]] auto num_light_grids = kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize;
    assert(UB->LightGridNumCascadeGrids * UB->LighGridNumCascadesUsed == num_light_grids);
    UB->LightGridNumGrids = UB->LightGridNumCascadeGrids * UB->LighGridNumCascadesUsed;
    UB->LightInjectionIntensityThreshold = CVar_LightInjectionIntensityThreshold.Get();
    for (int i = 0; i < kLightGridNumCascades; i++) {
        auto min = camera_pos + 0.5f * glm::vec3(-grid_cell_size, -grid_cell_size, -grid_cell_size) * float(kLightGridSize << i);
        auto max = camera_pos + 0.5f * glm::vec3(grid_cell_size, grid_cell_size, grid_cell_size) * float(kLightGridSize << i);
        UB->LightGridCascadeMin[i] = glm::vec4(min, 1.0f);
        UB->LightGridCascadeMax[i] = glm::vec4(max, 1.0f);
    }
    if (CVar_DebugFreezeFrameSeed.Get()) UB->FrameIndex = 0;
    else UB->FrameIndex = view->persistent_data_->frame_index_;
    UB->MaxNumLights = (uint32_t)max_num_lights;

    auto num_env_mips = view->scene_->GetSkyTexture()->GetMipLevels();
    UB->EnvironmentLightHemisphereSampleLOD = std::max(float(num_env_mips) - 1.75f, 0.f);
}

MI_NAMESPACE_END