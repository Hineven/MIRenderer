/*
 * Created: 2025/7/25
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer.h"
#include "r_view_common.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_resource_allocator.h"
#include "../shaders/shared/SharedLight.hlsl"
#include "renderer/mi_scene.h"
MI_NAMESPACE_BEGIN
static constexpr uint32_t kLightGridSize = 16;
static constexpr uint32_t kLightGridNumCascades = 6; // Number of cascades in the light grid

static CVar<int> CVar_MaxNumGridLights(
    "r.lightgrid.max_num_grid_lights",
    "Maximum number of lights in each grid cell.",
    32
);

static CVar<int> CVar_NumLightSamplerSamples(
    "r.lightgrid.num_light_sampler_samples",
    "Number of candidate samples to take when sampling lights in the light grid.",
    8
);

static CVar<int> CVar_MaxNumLightGridEntries(
    "r.lightgrid.max_num_entries",
    "Maximum number of entries in the light grid.",
    1024 * 1024
);

static CVar<float> CVar_MinLightGridSize(
    "r.lightgrid.min_size",
    "Minimum size of the light grid in world units.",
    0.4f
);

static CVar<float> CVar_ShadowRayLengthMultiplier(
    "r.lightgrid.shadow_ray_length_multiplier",
    "Multiplier for the shadow ray length for direct lighting occlusion tests.",
    0.995f
);

static CVar<float> CVar_LightInjectionIntensityThreshold(
    "r.lightgrid.light_injection_intensity_threshold",
    "Threshold for light injection intensity. Lights with intensity below this value will not be injected into the grid.",
    0.001f
);

static constexpr uint32_t kThreadGroupSize = 128;

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
struct DirectLightingUB {
    uint32_t FrameIndex;
    float ShadowRayTMax;
    float ShadowRayLengthMultiplier;
    uint32_t Unused;
};

BEGIN_SHADER_PARAMETERS(DirectLightingShaderParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructured_UB)
    SHADER_UNIFORM_BUFFER(DirectLightingUB, DirectLighting_UB)
    SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWPrecomputedActiveLightBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrecomputedActiveLightBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveLightListCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActiveLightListBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveLightListCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveLightListBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_ListLightIndexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_GridLightListOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_GridLightListCdfBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_GridLightListLengthBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_BloomFilterBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLightGrid_ListAllocatorBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLightGrid_ListLightIndexBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLightGrid_GridLightListCdfBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLightGrid_GridLightListOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLightGrid_GridLightListLengthBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWLightGrid_BloomFilterBuffer)
END_SHADER_PARAMETERS()



class ClearLightGridShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize),
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(ClearLightGridShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "ClearLightGrid");

class PrecomputeLightsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize),
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(PrecomputeLightsShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "PrecomputeLights");

class InjectLightsShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize),
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(InjectLightsShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "InjectLights");

class SpawnLightSamplesShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize),
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(SpawnLightSamplesShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "SpawnLightSamples");

class ScreenSpaceTraceForDirectLightingShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize),
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(ScreenSpaceTraceForDirectLightingShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "ScreenSpaceTraceForDirectLighting");

class RenderDiffuseDirectLightingShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize),
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(RenderDiffuseDirectLightingShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "RenderDiffuseDirectLighting");

static RDGShaderInitializationInfo GetDirectLightingShaderInitializationInfo() {
    RDGShaderInitializationInfo ini {};
    ini.optional_macros = {
        "MAX_NUM_GRID_LIGHTS=" + std::to_string(CVar_MaxNumGridLights.Get()),
        "NUM_LIGHT_SAMPELR_SAMPLES=" + std::to_string(CVar_NumLightSamplerSamples.Get())
    };
    return ini;
}

void Renderer::Render_ComputeDirectLighting(RendererView *view, RenderGraphBuilder &builder) {
    auto & lib = RDGShaderLibrary::Get();
    auto ini = GetDirectLightingShaderInitializationInfo();

    auto params = builder.Allocate<DirectLightingShaderParameters>();
    auto light_buffer = builder.Import(device_allocator_->GetAreaLightsUberBuffer()->GetRHI());
    auto max_num_lights = device_allocator_->GetAreaLightsUberBuffer()->GetAllocationLimitByteOffset() / sizeof(RawLight);
    auto precomputed_active_light_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_lights * sizeof(PackedPrecomputedLight)
    );
    auto active_light_list_count = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );
    auto active_light_list_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_lights * sizeof(uint32_t)
    );
    auto max_num_light_grid_entries = CVar_MaxNumLightGridEntries.Get();
    auto num_light_grids = kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize;
    auto light_grid_list_light_index_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_light_grid_entries * sizeof(uint32_t)
    );
    auto light_grid_grid_light_list_offset_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_light_grids * sizeof(uint32_t)
    );
    auto light_grid_grid_light_list_cdf_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_light_grids * sizeof(float)
    );
    auto light_grid_grid_light_list_length_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_light_grids * sizeof(uint32_t)
    );
    auto light_grid_bloom_filter_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_light_grid_entries * sizeof(uint32_t) * 4
    );
    auto light_grid_list_allocator_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );

    auto ray_to_trace_count = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t));


    {
        params->View = view->view_common_params_;
        auto L_UB = builder.Allocate<LightStructureUB>();
        {
            auto scene_aabb = view->scene_->GetAABB();
            auto camera_pos = view->camera_.position;
            auto dist_to_min = scene_aabb.min - camera_pos;
            auto dist_to_max = scene_aabb.max - camera_pos;
            auto maxv = glm::max(glm::abs(dist_to_min), glm::abs(dist_to_max));
            auto hmaxv = std::max(maxv.x, std::max(maxv.y, maxv.z));
            float grid_cell_size = float(double(hmaxv) / pow(2, kLightGridNumCascades) / kLightGridSize);
            L_UB->LightGridSize = glm::uvec3(kLightGridSize);
            L_UB->LightGridCellSize = grid_cell_size;
            L_UB->LightGridCenter = camera_pos;
            L_UB->LighGridNumCascadesUsed = kLightGridNumCascades;
            L_UB->LightGridMaxNumGridLights = CVar_MaxNumGridLights.Get();
            L_UB->LightGridNumCascadeGrids = kLightGridSize * kLightGridSize * kLightGridSize;
            assert(L_UB->LightGridNumCascadeGrids * L_UB->LighGridNumCascadesUsed == num_light_grids);
            L_UB->LightGridNumGrids = L_UB->LightGridNumCascadeGrids * L_UB->LighGridNumCascadesUsed;
            L_UB->LightInjectionIntensityThreshold = CVar_LightInjectionIntensityThreshold.Get();
            for (int i = 0; i < kLightGridNumCascades; i++) {
                auto min = camera_pos + 0.5f * glm::vec3(-grid_cell_size, -grid_cell_size, -grid_cell_size) * float(1 << i);
                auto max = camera_pos + 0.5f * glm::vec3(grid_cell_size, grid_cell_size, grid_cell_size) * float(1 << i);
                L_UB->LightGridCascadeMin[i] = glm::vec4(min, 1.0f);
                L_UB->LightGridCascadeMax[i] = glm::vec4(max, 1.0f);
            }
            L_UB->FrameIndex = view->persistent_data_->view_index;
            L_UB->MaxNumLights = (uint32_t)max_num_lights;
        }
        params->LightStructured_UB = L_UB;
        auto DI_UB = builder.Allocate<DirectLightingUB>();
        {
            DI_UB->FrameIndex = view->persistent_data_->view_index;
            DI_UB->ShadowRayTMax = view->camera_.far_plane;
            DI_UB->ShadowRayLengthMultiplier = CVar_ShadowRayLengthMultiplier.Get();
        }
        params->DirectLighting_UB = DI_UB;
        params->LightBuffer = light_buffer;
        params->RWPrecomputedActiveLightBuffer = precomputed_active_light_buffer.Raw();
        params->PrecomputedActiveLightBuffer = precomputed_active_light_buffer.Raw();
        params->RWActiveLightListCount = active_light_list_count.Raw();
        params->RWActiveLightListBuffer = active_light_list_buffer.Raw();
        params->ActiveLightListCount = active_light_list_count.Raw();
        params->ActiveLightListBuffer = active_light_list_buffer.Raw();
        params->LightGrid_ListLightIndexBuffer = light_grid_list_light_index_buffer.Raw();
        params->LightGrid_GridLightListOffsetBuffer = light_grid_grid_light_list_offset_buffer.Raw();
        params->LightGrid_GridLightListCdfBuffer = light_grid_grid_light_list_cdf_buffer.Raw();
        params->LightGrid_GridLightListLengthBuffer = light_grid_grid_light_list_length_buffer.Raw();
        params->LightGrid_BloomFilterBuffer = light_grid_bloom_filter_buffer.Raw();
        params->RWLightGrid_ListAllocatorBuffer = light_grid_list_allocator_buffer.Raw();
        params->RWLightGrid_ListLightIndexBuffer = light_grid_list_light_index_buffer.Raw();
        params->RWLightGrid_GridLightListCdfBuffer = light_grid_grid_light_list_cdf_buffer.Raw();
        params->RWLightGrid_GridLightListOffsetBuffer = light_grid_grid_light_list_offset_buffer.Raw();
        params->RWLightGrid_GridLightListLengthBuffer = light_grid_grid_light_list_length_buffer.Raw();
        params->RWLightGrid_BloomFilterBuffer = light_grid_bloom_filter_buffer.Raw();

        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    }
    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
    // 1. Clear counters
    {
        auto shader = lib.GetShader<ClearLightGridShader>(ini);
        auto num_groups = DivideAndRoundUp(num_light_grids, kThreadGroupSize);
        Helpers::DispatchComputePass<ClearLightGridShader>(
            builder, shader, params, num_groups
        );
    }
    // 2. Precompute lights
    {
        auto shader = lib.GetShader<PrecomputeLightsShader>(ini);
        auto num_groups = DivideAndRoundUp(max_num_lights, kThreadGroupSize);
        Helpers::DispatchComputePass<PrecomputeLightsShader>(
            builder, shader, params, (uint32_t)num_groups
        );
    }
    {
        auto shader = lib.GetShader<InjectLightsShader>(ini);
        auto num_groups = DivideAndRoundUp(num_light_grids, wave_size);
        Helpers::DispatchComputePass<InjectLightsShader>(
            builder, shader, params, num_groups
        );
    }
    auto tile_size = 8;
    {
        auto shader = lib.GetShader<SpawnLightSamplesShader>(ini);
        auto num_groups_x = DivideAndRoundUp(view->film_width_, tile_size);
        auto num_groups_y = DivideAndRoundUp(view->film_height_, tile_size);
        Helpers::DispatchComputePass<SpawnLightSamplesShader>(
            builder, shader, params, num_groups_x, num_groups_y
        );
    }
    auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, ray_to_trace_count.Raw(), wave_size);
    {
        auto shader = lib.GetShader<ScreenSpaceTraceForDirectLightingShader>(ini);
        Helpers::DispatchIndirectComputePass<ScreenSpaceTraceForDirectLightingShader>(
            builder, shader, params, cmd.Raw()
        );
    }
    {
        // HWRT...
    }
    {
        auto shader = lib.GetShader<RenderDiffuseDirectLightingShader>(ini);
        Helpers::DispatchIndirectComputePass<RenderDiffuseDirectLightingShader>(
            builder, shader, params, cmd.Raw()
        );
    }
}


MI_NAMESPACE_END