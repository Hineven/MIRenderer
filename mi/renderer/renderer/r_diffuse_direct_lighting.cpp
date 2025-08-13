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
    0.15f
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
struct HybridTracingUB {
    float SSRT_RelativeTexelThickness;
    float RayContinuationBackwardBiasFactor;
    float DefaultTMax;
    uint32_t Unused2;
};

BEGIN_SHADER_PARAMETERS(DirectLightingShaderParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)
    SHADER_UNIFORM_BUFFER(DirectLightingUB, DirectLighting_UB)
    SHADER_UNIFORM_BUFFER(HybridTracingUB, HybridTracing_UB)
    SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointClampSampler)
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

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceListAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceOriginScreenCoordBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadowRayToTraceTMaxBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ShadowRayToTraceTMaxBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_DepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_NormalTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_HiZBuffer)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_HistoryDepth)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDiffuseDirectLightingTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDirectLightingRayIndexTexture)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDirectLightingRadianceEstimateTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, DirectLightingRadianceEstimateTexture)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(DirectLightingShaderParameters)

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

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearLightGridShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "ClearLightGrid");

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

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(PrecomputeLightsShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "PrecomputeLights");

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

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(InjectLightsShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "InjectLights");

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

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(SpawnLightSamplesShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "SpawnLightSamples");

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

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ScreenSpaceTraceForDirectLightingShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "ScreenSpaceTraceForDirectLighting");

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

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(RenderDiffuseDirectLightingShader, "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "RenderDiffuseDirectLighting");

static RDGShaderInitializationInfo GetDirectLightingShaderInitializationInfo() {
    RDGShaderInitializationInfo ini {};
    ini.optional_macros = {
        "MAX_NUM_GRID_LIGHTS=" + std::to_string(CVar_MaxNumGridLights.Get()),
        "NUM_LIGHT_SAMPELR_SAMPLES=" + std::to_string(CVar_NumLightSamplerSamples.Get())
    };
    return ini;
}

BEGIN_SHADER_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)
    SHADER_UNIFORM_BUFFER(DirectLightingUB, DirectLighting_UB)
    SHADER_UNIFORM_BUFFER(HybridTracingUB, HybridTracing_UB)
    SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointClampSampler)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrecomputedActiveLightBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveLightListCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActiveLightListBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_ListLightIndexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_GridLightListOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_GridLightListCdfBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_GridLightListLengthBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_BloomFilterBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeSampleColorAndLinearDepth)
    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeSampleTransmittanceAndPdf)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeDirectLightingRadianceEstimateTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeDirectLightingRadianceEstimateTexture)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_DepthTexture)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceOriginBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceTMaxBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTracePixelIndexBuffer)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumeRayToTraceTransmittanceBuffer)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeDirectLightingTexture)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)

class VolumePrimitivesSpawnLightSamplesShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize),
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
    VolumePrimitivesSpawnLightSamplesShader,
    "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "VolumePrimitivesSpawnLightSamples");

class RenderVolumeDirectLightingShader : public RDGShader {
public:
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize),
        };
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
    RenderVolumeDirectLightingShader,
    "mi/renderer/shaders/DiffuseDirectLighting.hlsl", "RenderVolumeDirectLighting");

void Renderer::Render_ComputeDirectLighting(RendererView *view, RenderGraphBuilder &builder) {
    auto & lib = RDGShaderLibrary::Get();
    auto ini = GetDirectLightingShaderInitializationInfo();

    auto params = builder.Allocate<DirectLightingShaderParameters>();
    auto light_buffer = builder.Import(device_allocator_->GetAreaLightsUberBuffer()->GetRHI());
    auto max_num_lights = device_allocator_->GetAreaLightsUberBuffer()->GetAllocationLimitByteOffset() / sizeof(RawLight);
    auto precomputed_active_light_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_lights * sizeof(PackedPrecomputedLight)
    );
    precomputed_active_light_buffer->SetName("PrecomputedActiveLightBuffer");
    auto active_light_list_count = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );
    active_light_list_count->SetName("ActiveLightListCount");
    auto active_light_list_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_lights * sizeof(uint32_t)
    );
    active_light_list_buffer->SetName("ActiveLightListBuffer");
    auto max_num_light_grid_entries = CVar_MaxNumLightGridEntries.Get();
    auto num_light_grids = kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize;
    auto light_grid_list_light_index_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_light_grid_entries * sizeof(uint32_t)
    );
    light_grid_list_light_index_buffer->SetName("LightGrid_ListLightIndexBuffer");
    auto light_grid_grid_light_list_offset_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_light_grids * sizeof(uint32_t)
    );
    light_grid_grid_light_list_offset_buffer->SetName("LightGrid_GridLightListOffsetBuffer");
    auto light_grid_grid_light_list_cdf_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_light_grids * sizeof(float)
    );
    light_grid_grid_light_list_cdf_buffer->SetName("LightGrid_GridLightListCdfBuffer");
    auto light_grid_grid_light_list_length_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_light_grids * sizeof(uint32_t)
    );
    light_grid_grid_light_list_length_buffer->SetName("LightGrid_GridLightListLengthBuffer");
    auto light_grid_bloom_filter_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, max_num_light_grid_entries * sizeof(uint32_t) * 4
    );
    light_grid_bloom_filter_buffer->SetName("LightGrid_BloomFilterBuffer");
    auto light_grid_list_allocator_buffer = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );
    light_grid_list_allocator_buffer->SetName("LightGrid_ListAllocatorBuffer");

    uint32_t num_screen_pixels = view->film_width_ * view->film_height_;

    auto ray_to_trace_count = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t));\
    ray_to_trace_count->SetName("RayToTraceCount");

    auto ray_to_trace_list_allocator = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );
    ray_to_trace_list_allocator->SetName("RayToTraceListAllocator");
    auto ray_to_trace_list = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(uint32_t)
    );
    ray_to_trace_list->SetName("RayToTraceList");
    auto ray_to_trace_direction = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(glm::vec3)
    );
    ray_to_trace_direction->SetName("RayToTraceDirection");
    auto ray_to_trace_state = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(uint32_t)
    );
    ray_to_trace_state->SetName("RayToTraceState");
    auto ray_to_trace_origin_screen_coords = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(glm::uvec2)
    );
    ray_to_trace_origin_screen_coords->SetName("RayToTraceOriginScreenCoord");

    auto volume_ray_to_trace_count = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );
    volume_ray_to_trace_count->SetName("VolumeRayToTraceCount");

    auto direct_lighting_radiance_estimate_texture = builder.CreateTexture(
    RHITextureDesc{RHITextureType::k2D, RHITextureDimensions {view->film_width_, view->film_height_, 1},
        1, 1, PixelFormatType::kR16G16B16A16_FLOAT, RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource});
    direct_lighting_radiance_estimate_texture->SetName("DirectLightingRadianceEstimateTexture");
    auto direct_lighting_ray_index_texture = builder.CreateTexture(
    RHITextureDesc{RHITextureType::k2D, RHITextureDimensions {view->film_width_, view->film_height_, 1},
1, 1, PixelFormatType::kR32_UINT, RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource}
    );
    direct_lighting_ray_index_texture->SetName("DirectLightingRayIndexTexture");

    auto shadow_ray_to_trace_tmax = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(float)
    );
    shadow_ray_to_trace_tmax->SetName("ShadowRayToTraceTMaxBuffer");
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
            float grid_cell_size = 4.01f * float(double(hmaxv) / pow(2, kLightGridNumCascades) / kLightGridSize);
            grid_cell_size = std::max(grid_cell_size, CVar_MinLightGridSize.Get());
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
                auto min = camera_pos + 0.5f * glm::vec3(-grid_cell_size, -grid_cell_size, -grid_cell_size) * float(kLightGridSize << i);
                auto max = camera_pos + 0.5f * glm::vec3(grid_cell_size, grid_cell_size, grid_cell_size) * float(kLightGridSize << i);
                L_UB->LightGridCascadeMin[i] = glm::vec4(min, 1.0f);
                L_UB->LightGridCascadeMax[i] = glm::vec4(max, 1.0f);
            }
            L_UB->FrameIndex = view->persistent_data_->frame_index_;
            L_UB->MaxNumLights = (uint32_t)max_num_lights;
        }
        params->LightStructure_UB = L_UB;
        auto DI_UB = builder.Allocate<DirectLightingUB>();
        {
            DI_UB->FrameIndex = view->persistent_data_->frame_index_;
            DI_UB->ShadowRayTMax = view->camera_.far_plane;
            DI_UB->ShadowRayLengthMultiplier = CVar_ShadowRayLengthMultiplier.Get();
        }
        params->DirectLighting_UB = DI_UB;
        auto HT_UB = builder.Allocate<HybridTracingUB>();
        {
            HT_UB->SSRT_RelativeTexelThickness = 1e-4f;
            HT_UB->RayContinuationBackwardBiasFactor = 1e-3f;
            HT_UB->DefaultTMax = view->camera_.far_plane;
        }
        params->HybridTracing_UB = HT_UB;
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

        params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
        params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
        params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
        params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
        params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
        params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
        params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());

        params->RWRayToTraceCount = ray_to_trace_count.Raw();
        params->RayToTraceCount = ray_to_trace_count.Raw();
        params->RWVolumeRayToTraceCount = volume_ray_to_trace_count.Raw();
        params->RWRayToTraceListAllocator = ray_to_trace_list_allocator.Raw();
        params->RWRayToTraceListBuffer = ray_to_trace_list.Raw();
        params->RWRayToTraceDirectionBuffer = ray_to_trace_direction.Raw();
        params->RWRayToTraceStateBuffer = ray_to_trace_state.Raw();
        params->RWRayToTraceOriginScreenCoordBuffer = ray_to_trace_origin_screen_coords.Raw();

        params->RWShadowRayToTraceTMaxBuffer = shadow_ray_to_trace_tmax.Raw();
        params->ShadowRayToTraceTMaxBuffer = shadow_ray_to_trace_tmax.Raw();

        params->G_DepthTexture = view->G_depth_.Raw();
        params->G_NormalTexture = view->G_normal_.Raw();
        params->G_HiZBuffer = view->hzb_.Raw();
        params->G_HistoryDepth = view->persistent_data_->prev_G_depth.Raw();
        params->RWDiffuseDirectLightingTexture = view->diffuse_direct_lighting_.Raw();
        params->RWDirectLightingRayIndexTexture = direct_lighting_ray_index_texture.Raw();
        params->RWDirectLightingRadianceEstimateTexture = direct_lighting_radiance_estimate_texture.Raw();
        params->DirectLightingRadianceEstimateTexture = direct_lighting_radiance_estimate_texture.Raw();

        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
        params->PointClampSampler = RHI::Get().GetGlobalSamplers().point_clamp;
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
        // HWRT
        Render_HardwareShadowRayTracing(
            view, builder,
            ray_to_trace_list_allocator.Raw(),
            ray_to_trace_list.Raw(),
            ray_to_trace_direction.Raw(),
            ray_to_trace_state.Raw(),
            ray_to_trace_origin_screen_coords.Raw(),
            nullptr,
            shadow_ray_to_trace_tmax.Raw()
        );
    }
    {
        auto shader = lib.GetShader<RenderDiffuseDirectLightingShader>(ini);
        Helpers::Clear(builder, view->diffuse_direct_lighting_.Raw());
        Helpers::DispatchIndirectComputePass<RenderDiffuseDirectLightingShader>(
            builder, shader, params, cmd.Raw()
        );
    }

    auto volprims_params = builder.Allocate<VolumePrimitivesDirectLightingShaderParameters>();
    volprims_params->View = view->view_common_params_;
    volprims_params->LightStructure_UB = params->LightStructure_UB;
    volprims_params->DirectLighting_UB = params->DirectLighting_UB;
    volprims_params->HybridTracing_UB = params->HybridTracing_UB;

    volprims_params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    volprims_params->PointClampSampler = RHI::Get().GetGlobalSamplers().point_clamp;

    volprims_params->LightBuffer = params->LightBuffer;
    volprims_params->PrecomputedActiveLightBuffer = params->PrecomputedActiveLightBuffer;
    auto volume_ray_to_trace_list_allocator = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t)
    );
    auto volume_ray_to_trace_list = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(uint32_t)
    );
    auto volume_ray_to_trace_direction = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(glm::vec3)
    );
    auto volume_ray_to_trace_origins = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(glm::uvec2)
    );
    auto volume_ray_to_trace_state = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(uint)
    );
    auto volume_ray_to_trace_tmax = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(float)
    );
    volprims_params->ActiveLightListCount = params->ActiveLightListCount;
    volprims_params->ActiveLightListBuffer = params->ActiveLightListBuffer;
    volprims_params->LightGrid_ListLightIndexBuffer = params->LightGrid_ListLightIndexBuffer;
    volprims_params->LightGrid_GridLightListOffsetBuffer = params->LightGrid_GridLightListOffsetBuffer;
    volprims_params->LightGrid_GridLightListCdfBuffer = params->LightGrid_GridLightListCdfBuffer;
    volprims_params->LightGrid_GridLightListLengthBuffer = params->LightGrid_GridLightListLengthBuffer;
    volprims_params->LightGrid_BloomFilterBuffer = params->LightGrid_BloomFilterBuffer;

    volprims_params->VolumeSampleColorAndLinearDepth = view->volume_sample_color_and_linear_depth_.Raw();
    volprims_params->VolumeSampleTransmittanceAndPdf = view->volume_sample_transmittance_and_pdf_.Raw();
    auto volume_di_radiance_estimate_texture = builder.CreateTexture(
        RHITextureDesc{RHITextureType::k2D, RHITextureDimensions {view->film_width_, view->film_height_, 1},
            1, 1, PixelFormatType::kR16G16B16A16_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource}
    );

    volprims_params->RWVolumeDirectLightingRadianceEstimateTexture = volume_di_radiance_estimate_texture.Raw();
    volprims_params->VolumeDirectLightingRadianceEstimateTexture = volume_di_radiance_estimate_texture.Raw();

    volprims_params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    volprims_params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
    volprims_params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    volprims_params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    volprims_params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    volprims_params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    volprims_params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());

    volprims_params->G_DepthTexture = view->G_depth_.Raw();

    volprims_params->RWVolumeRayToTraceCount = volume_ray_to_trace_count.Raw();
    volprims_params->RWVolumeRayToTraceListBuffer = volume_ray_to_trace_list.Raw();
    volprims_params->RWVolumeRayToTraceDirectionBuffer = volume_ray_to_trace_direction.Raw();
    volprims_params->RWVolumeRayToTraceStateBuffer = volume_ray_to_trace_state.Raw();
    volprims_params->RWVolumeRayToTraceOriginBuffer = volume_ray_to_trace_origins.Raw();
    volprims_params->RWVolumeRayToTraceTMaxBuffer = volume_ray_to_trace_tmax.Raw();

    auto volume_ray_to_trace_pixel_index = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(uint32_t)
    );
    volprims_params->RWVolumeRayToTracePixelIndexBuffer = volume_ray_to_trace_pixel_index.Raw();
    auto volume_ray_to_trace_transmittance = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(float)
    );
    volprims_params->VolumeRayToTraceTransmittanceBuffer = volume_ray_to_trace_transmittance.Raw();

    volprims_params->RWVolumeDirectLightingTexture = view->volume_direct_lighting_.Raw();

    // Volume Direct Lighting
    {
        auto shader = lib.GetShader<VolumePrimitivesSpawnLightSamplesShader>(ini);
        auto num_groups_x = DivideAndRoundUp(view->film_width_, tile_size);
        auto num_groups_y = DivideAndRoundUp(view->film_height_, tile_size);
        Helpers::DispatchComputePass<VolumePrimitivesSpawnLightSamplesShader>(
            builder, shader, volprims_params, num_groups_x, num_groups_y
        );
    }

    {
        // HWRT
        Render_HardwareTransmittanceRayTracing(
            view, builder,
            volume_ray_to_trace_list_allocator.Raw(),
            volume_ray_to_trace_list.Raw(),
            volume_ray_to_trace_direction.Raw(),
            volume_ray_to_trace_state.Raw(),
            nullptr,
            volume_ray_to_trace_origins.Raw(),
            volume_ray_to_trace_tmax.Raw(),
            volume_ray_to_trace_transmittance.Raw()
        );
    }

    cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, volume_ray_to_trace_count.Raw(), wave_size);

    {
        auto shader = lib.GetShader<RenderVolumeDirectLightingShader>(ini);
        Helpers::Clear(builder, view->volume_direct_lighting_.Raw());
        Helpers::DispatchIndirectComputePass<RenderVolumeDirectLightingShader>(
            builder, shader, volprims_params, cmd.Raw()
        );
    }
}


MI_NAMESPACE_END