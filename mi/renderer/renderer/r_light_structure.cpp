/*
 * Created: 2025/10/10
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <rdg/rdg_helper.h>
#include <renderer/mi_renderer.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_scene.h>
#include <renderer/mi_texture.h>
#include <renderer/mi_buffer_heap.h>
#include <renderer/mi_static_mesh.h>

#include "r_persistent.h"
#include "r_light_structure.h"
#include "../shaders/shared/SharedLight.hlsl"
#include "../shaders/shared/SharedLightClusterHierarchy.hlsl"
MI_NAMESPACE_BEGIN
constexpr uint32_t kLightPrecomputationLevelsPerDispatch = 3;

CVar<int> CVar_MaxNumGridLights(
    "r.lightgrid.max_num_grid_lights",
    "Maximum number of lights in each grid cell.",
    12
);
CVar<int> CVar_MaxNumLightGridEntries(
    "r.lightgrid.max_num_entries",
    "Maximum number of entries in the light grid.",
    2 * 1024 * 1024
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

static CVar<int> CVar_MaxNumActiveLightGrids(
    "r.lightgrid.max_num_active_grids",
    "Maximum number of active light grids. Active grids are those that have lights injected and are considered for sampling.",
    256 * 1024
);

CVar<int> CVar_NumLightSamplerSamples(
    "r.lightgrid.num_light_sampler_samples",
    "Number of candidate samples to take when sampling lights in the light grid.",
    8
);

CVar<float> CVar_LightCullingRate(
    "r.lightgrid.light_culling_rate",
    "Culling rate for lights to be injected into the light grid. Higher values will result in less injected lights, but can increase noise.",
    0.0f
);

CVar<glm::vec3> CVar_EnvironmentLightMultiplier(
    "r.lightgrid.environment_light_multiplier",
    "Multiplier for environment light contribution in the light grid.",
    glm::vec3{1.0f}
);

CVar<float> CVar_EnvironmentLightEvaluateLOD(
    "r.lightgrid.environment_light_evaluate_lod",
    "LOD at which to evaluate environment light contribution in the light grid. Higher values will result in blurrier environment lighting but better performance.",
    0.0f
);

static CVar<bool> CVar_DebugFreezeFrameSeed(
    "r.lightgrid.debug.freeze_frame_seed",
    "Freeze the frame index used for random seed generation when injecting lights. This is useful for debugging.",
    false
);

bool LightStructurePersistentData::MakeSureExists([[maybe_unused]] RendererView *view, RenderGraphBuilder &builder) {
    bool flag = false;
    auto num_light_grids = kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize;
    if (!environment_visibility_history_buffer) {
        environment_visibility_history_buffer = builder.CreateBuffer<uint32_t>(num_light_grids * kLightGridNumHistories);
        environment_visibility_history_buffer->SetName("LightGrid_RWEnvironmentVisibilityHistoryBuffer");
        environment_visibility_history_buffer->SetExport();
        flag = true;
    }
    if (!bloom_filter_buffer) {
        bloom_filter_buffer = builder.CreateBuffer<glm::uvec2>(num_light_grids * kLightGridNumHistories);
        bloom_filter_buffer->SetName("LightGrid_RWBloomFilterBuffer");
        bloom_filter_buffer->SetExport();
        flag = true;
    }
    if (!active_grid_flag_buffer) {
        active_grid_flag_buffer = builder.CreateBuffer<uint32_t>(num_light_grids);
        active_grid_flag_buffer->SetName("LightGrid_RWActiveGridFlagBuffer");
        active_grid_flag_buffer->SetExport();
        flag = true;
    }
    if (!grid_pressure_buffer) {
        grid_pressure_buffer = builder.CreateBuffer<glm::uvec2>(num_light_grids);
        grid_pressure_buffer->SetName("LightGrid_RWGridPressureBuffer");
        grid_pressure_buffer->SetExport();
        flag = true;
    }
    need_reset_ |= flag;
    return flag;
}

void LightStructurePersistentData::FinalUpdate([[maybe_unused]] RendererView *view) {
    // No staged buffers, nothing to do here
}


void LightStructureData::Allocate(RenderGraphBuilder &builder) {
    auto & r = Renderer::Get();
    auto max_num_lights = r.GetDeviceAllocator()->GetAreaLightsUberBuffer()->GetAllocationLimitByteOffset() / sizeof(RawLight);
    auto num_light_grids = kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize;

    active_grid_count = builder.CreateBuffer<uint32_t>();
    active_grid_count->SetName("LightGrid_RWActiveGridAllocator");
    active_grid_indices_buffer = builder.CreateBuffer<uint32_t>(num_light_grids);
    active_grid_indices_buffer->SetName("LightGrid_RWActiveGridIndicesBuffer");
    active_mesh_light_instance_count = builder.CreateBuffer<uint32_t>();
    active_mesh_light_instance_count->SetName("LightGrid_RWActiveMeshLightInstanceCount");
    active_light_list_count = builder.CreateBuffer<uint32_t>();
    active_light_list_count->SetName("LightGrid_RWActiveLightListCount");
    active_light_list_buffer = builder.CreateBuffer<uint32_t>(max_num_lights);
    active_light_list_buffer->SetName("LightGrid_RWActiveLightListBuffer");
    auto max_num_light_grid_entries = CVar_MaxNumLightGridEntries.Get();
    list_active_light_list_index_buffer = builder.CreateBuffer<uint32_t>(max_num_light_grid_entries);
    list_mesh_light_instance_element_index_buffer = builder.CreateBuffer<uint32_t>(max_num_light_grid_entries);
    list_allocator = builder.CreateBuffer<uint32_t>();
    list_allocator->SetName("LightGrid_RWListAllocator");
    list_active_light_list_index_buffer->SetName("LightGrid_RWListActiveLightListIndexBuffer");
    list_mesh_light_instance_element_index_buffer->SetName("LightGrid_RWListMeshLightInstanceElementIndexBuffer");
    grid_light_list_offset_buffer = builder.CreateBuffer<uint32_t>(num_light_grids);
    grid_light_list_offset_buffer->SetName("LightGrid_RWGridLightListOffsetBuffer");
    grid_light_list_cdf_buffer = builder.CreateBuffer<float>(num_light_grids);
    grid_light_list_cdf_buffer->SetName("LightGrid_RWGridLightListCdfBuffer");
    grid_light_list_length_buffer = builder.CreateBuffer<uint32_t>(num_light_grids);
    grid_light_list_length_buffer->SetName("LightGrid_RWGridLightListLengthBuffer");

    next_bloom_filter_buffer = builder.CreateBuffer<glm::uvec2>(num_light_grids);
    next_bloom_filter_buffer->SetName("LightGrid_RWNextBloomFilterBuffer");
    next_environment_visibility_buffer = builder.CreateBuffer<uint32_t>(num_light_grids);
    next_environment_visibility_buffer->SetName("LightGrid_RWNextEnvironmentVisibilityBuffer");
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
    UB->LightGridMaxNumEntries = std::max(CVar_MaxNumLightGridEntries.Get(), 1);

    if (auto env = view->scene_->GetSkyTexture()) {
        auto num_env_mips = env->GetMipLevels();
        UB->EnvironmentLightHemisphereSampleLOD = std::max(float(num_env_mips) - 1.75f, 0.f);
    } else {
        UB->EnvironmentLightHemisphereSampleLOD = 0;
    }
    UB->LightCullingRate = CVar_LightCullingRate.Get();

    UB->EnvironmentLightMultiplier = glm::max(glm::vec3{0.f}, CVar_EnvironmentLightMultiplier.Get());
    UB->EnvironmentLightEvaluateLOD = CVar_EnvironmentLightEvaluateLOD.Get();
    UB->NumMLIClusters = view->light_structure_ ? view->light_structure_->num_active_mesh_light_instance_clusters_ : 0;
    UB->MaxNumActiveLightGrids = CVar_MaxNumActiveLightGrids.Get();
    UB->padding = 0;
}

std::vector<std::string> GetLightStructureShaderMacros () {
    return {
        "MAX_NUM_GRID_LIGHTS=" + std::to_string(CVar_MaxNumGridLights.Get()),
        "NUM_LIGHT_SAMPLER_SAMPLES=" + std::to_string(CVar_NumLightSamplerSamples.Get()),
        "LIGHT_GRID_NUM_HISTORY_FRAMES=" + std::to_string(kLightGridNumHistories),
        "PROCESSING_LEVELS_PER_DISPATCH=" + std::to_string(kLightPrecomputationLevelsPerDispatch)
    };
}

class ClearLightStructureHistoryShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWBloomFilterBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWEnvironmentVisibilityHistoryBuffer)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
    constexpr static uint32_t kThreadGroupSize = 128;
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)
        };
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return GetLightStructureShaderMacros();
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(ClearLightStructureHistoryShader, "mi/renderer/shaders/LightStructure.hlsl", "ClearLightStructureHistory");

class UpdateLightStructureHistoryShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWBloomFilterBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWEnvironmentVisibilityHistoryBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWNextBloomFilterBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWNextEnvironmentVisibilityBuffer)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
    constexpr static uint32_t kThreadGroupSize = 128;
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)
        };
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return GetLightStructureShaderMacros();
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(UpdateLightStructureHistoryShader, "mi/renderer/shaders/LightStructure.hlsl", "UpdateLightStructureHistory");

struct LightPrecomputationLevelUB {
    uint32_t LevelIndex;
    uint32_t padding[3];
};

BEGIN_SHADER_PARAMETERS(LightStructureParameters)
    SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)
    SHADER_UNIFORM_BUFFER(LightPrecomputationLevelUB, LightPrecomputation_LevelUB)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWActiveGridAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWActiveGridIndicesBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWActiveGridFlagBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWGridPressureBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWActiveMeshLightInstanceCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightGrid_ActiveMeshLightInstanceIndexBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWListAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWListMeshLightInstanceElementIndexBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWGridLightListOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWGridLightListCdfBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWGridLightListLengthBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWEnvironmentVisibilityHistoryBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWBloomFilterBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWNextBloomFilterBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_RWNextEnvironmentVisibilityBuffer)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightTriangleBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightTriangleHashBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightTriangleBakedDataBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightClusterHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightClusterNodeBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightLevelHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightInstanceBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightInstanceClusterHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LCH_RWMeshLightInstanceClusterHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightInstanceClusterNodeBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LCH_RWMeshLightInstanceClusterNodeBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightInstanceTriangleBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LCH_RWMeshLightInstanceTriangleBuffer)
END_SHADER_PARAMETERS()
IMPLEMENT_SHADER_PARAMETERS(LightStructureParameters)

namespace {
    class PrecomputeLightStructureShader : public RDGShader {
    public:
        using RDGShader::RDGShader;
        RDG_SHADER_USE_PARAMETERS(LightStructureParameters)
        static std::vector<std::string> GetShaderDefaultMacros() {
            return {
                "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size)
            };
        }
        static std::vector<std::string> GetShaderOptionalMacros() {
            return GetLightStructureShaderMacros();
        }
    };

    class ClearCountersShader : public PrecomputeLightStructureShader {
    public:
        RDG_SHADER_USE_PARAMETERS(LightStructureParameters)
        DECLARE_SHADER(PrecomputeLightStructureShader)
        constexpr static uint32_t kThreadGroupSize = 128;
        static std::vector<std::string> GetShaderDefaultMacros() {
            auto macros = PrecomputeLightStructureShader::GetShaderDefaultMacros();
            macros.push_back("THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize));
            return macros;
        }
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearCountersShader, "mi/renderer/shaders/LightPrecomputation.hlsl", "LightPrecomputation_ClearCounters");

    class GatherActiveGridsShader : public PrecomputeLightStructureShader {
    public:
        RDG_SHADER_USE_PARAMETERS(LightStructureParameters)
        DECLARE_SHADER(PrecomputeLightStructureShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(GatherActiveGridsShader, "mi/renderer/shaders/LightPrecomputation.hlsl", "LightPrecomputation_GatherActiveGrids");

    class ClipActiveGridsShader : public PrecomputeLightStructureShader {
    public:
        RDG_SHADER_USE_PARAMETERS(LightStructureParameters)
        DECLARE_SHADER(PrecomputeLightStructureShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        ClipActiveGridsShader,
        "mi/renderer/shaders/LightPrecomputation.hlsl",
        "LightPrecomputation_ClipActiveGrids"
    );

    class ClearGridStatesShader : public PrecomputeLightStructureShader {
    public:
        RDG_SHADER_USE_PARAMETERS(LightStructureParameters)
        DECLARE_SHADER(PrecomputeLightStructureShader)
        constexpr static uint32_t kThreadGroupSize = 128;
        static std::vector<std::string> GetShaderDefaultMacros() {
            auto macros = PrecomputeLightStructureShader::GetShaderDefaultMacros();
            macros.push_back("THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize));
            return macros;
        }
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        ClearGridStatesShader,
        "mi/renderer/shaders/LightPrecomputation.hlsl",
        "LightPrecomputation_ClearGridStates"
    );

    class PrecomputeTrianglesShader : public PrecomputeLightStructureShader {
    public:
        DECLARE_SHADER(PrecomputeLightStructureShader)
        static RDGShaderPipelineConfig GetShaderPipelineConfig() {
            RDGShaderPipelineConfig cfg {};
            cfg.depth_test_enabled = false;
            cfg.depth_write_enabled = false;
            cfg.rasterization_discard = true; // We only need the shader to run for its side effects on buffers, no actual rasterization output.
            cfg.topology = RHIPrimitiveTopologyType::kPointList;
            return cfg;
        }
    };

    IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER(
        PrecomputeTrianglesShader,
        "mi/renderer/shaders/LightPrecomputation.hlsl",
        "LightPrecomputation_Triangle",
        ""
    );

    class PrecomputeLevelShader : public PrecomputeLightStructureShader {
    public:
        DECLARE_SHADER(PrecomputeLightStructureShader)
        static RDGShaderPipelineConfig GetShaderPipelineConfig() {
            RDGShaderPipelineConfig cfg {};
            cfg.depth_test_enabled = false;
            cfg.depth_write_enabled = false;
            cfg.rasterization_discard = true; // We only need the shader to run for its side effects on buffers, no actual rasterization output.
            cfg.topology = RHIPrimitiveTopologyType::kPointList;
            return cfg;
        }
    };

    IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER(
        PrecomputeLevelShader,
        "mi/renderer/shaders/LightPrecomputation.hlsl",
        "LightPrecomputation_Level",
        ""
    );

    class FinalizeMLIClustersShader : public PrecomputeLightStructureShader {
    public:
        DECLARE_SHADER(PrecomputeLightStructureShader)
        static RDGShaderPipelineConfig GetShaderPipelineConfig() {
            RDGShaderPipelineConfig cfg {};
            cfg.depth_test_enabled = false;
            cfg.depth_write_enabled = false;
            cfg.rasterization_discard = true;
            cfg.topology = RHIPrimitiveTopologyType::kPointList;
            return cfg;
        }
    };

    IMPLEMENT_RDG_GRAPHICS_SHADER_SHADER_SHARED_PARAMETER(
        FinalizeMLIClustersShader,
        "mi/renderer/shaders/LightPrecomputation.hlsl",
        "LightPrecomputation_FinalizeMLIClusters",
        ""
    );

    class InjectLightsShader : public PrecomputeLightStructureShader {
    public:
        RDG_SHADER_USE_PARAMETERS(LightStructureParameters)
        DECLARE_SHADER(PrecomputeLightStructureShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        InjectLightsShader,
        "mi/renderer/shaders/LightPrecomputation.hlsl",
        "LightPrecomputation_InjectLights"
    );
}

void Renderer::Render_PrepareLightStructure(RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    auto * ls = view->light_structure_.Raw();
    mi_check(ls, "Light structure data must exist before preparing the light structure.");

    auto & ls_ctx = ctx.light_structure;
    ls_ctx.active_mesh_light_instance_indices.clear();
    ls_ctx.triangle_draw_commands.clear();
    ls_ctx.finalize_cluster_draw_commands.clear();
    ls_ctx.level_draw_commands.clear();
    uint32_t num_active_mli_clusters = 0;
    uint32_t max_active_mli_levels = 0;

    for (auto renderable : ctx.visible_renderables) {
        auto * mesh_instance = renderable ? renderable->As<StaticMeshInstance>() : nullptr;
        if (!mesh_instance || mesh_instance->IsEmpty()) {
            continue;
        }

        auto * mli_alloc = mesh_instance->GetMeshLightInstanceBufferAllocation();
        if (!mli_alloc) {
            continue;
        }

        auto const & mesh_light_instances = mesh_instance->GetMeshLightInstances();
        auto * static_mesh = mesh_instance->GetStaticMesh();
        mi_check(static_mesh, "Visible static mesh instance must reference a static mesh.");
        auto const & hierarchy_records = static_mesh->GetLightHierarchyRecords();
        mi_check(
            mesh_light_instances.size() == hierarchy_records.size(),
            "Mesh light instance count must match light hierarchy record count while building light structure."
        );

        uint32_t mli_base_index = (uint32_t)(mli_alloc->GetRHI().offset / sizeof(MeshLightInstance));

        for (uint32_t local_mli_index = 0; local_mli_index < (uint32_t)mesh_light_instances.size(); ++local_mli_index) {
            auto const & hierarchy = hierarchy_records[local_mli_index].hierarchy;
            auto const & depth_level_cluster_counts = hierarchy_records[local_mli_index].depth_level_cluster_counts;
            auto const & mli = mesh_light_instances[local_mli_index];
            uint32_t active_list_index = (uint32_t)ls_ctx.active_mesh_light_instance_indices.size();
            ls_ctx.active_mesh_light_instance_indices.push_back(mli_base_index + local_mli_index);

            num_active_mli_clusters += (uint32_t)hierarchy.nodes.size();
            max_active_mli_levels = std::max(max_active_mli_levels, (uint32_t)depth_level_cluster_counts.size());

            if (!mli.MeshLightInstanceClusterOffset.bIsTriangle()) {
                ls_ctx.finalize_cluster_draw_commands.push_back(RHIDrawIndirectCommand {
                    (uint32_t)hierarchy.nodes.size(),
                    1,
                    0,
                    active_list_index
                });
            }

            if (!hierarchy.triangles.empty()) {
                ls_ctx.triangle_draw_commands.push_back(RHIDrawIndirectCommand {
                    (uint32_t)hierarchy.triangles.size(),
                    1,
                    0,
                    active_list_index
                });
            }

            uint32_t num_level_dispatches = DivideAndRoundUp((uint32_t)depth_level_cluster_counts.size(), kLightPrecomputationLevelsPerDispatch);
            if (ls_ctx.level_draw_commands.size() < num_level_dispatches) {
                ls_ctx.level_draw_commands.resize(num_level_dispatches);
            }
            for (uint32_t level_dispatch_index = 0; level_dispatch_index < num_level_dispatches; ++level_dispatch_index) {
                uint32_t level_index = level_dispatch_index * kLightPrecomputationLevelsPerDispatch;
                uint32_t level_cluster_count = depth_level_cluster_counts[level_index];
                if (level_cluster_count == 0) {
                    continue;
                }
                ls_ctx.level_draw_commands[level_dispatch_index].push_back(RHIDrawIndirectCommand {
                    level_cluster_count,
                    1,
                    0,
                    active_list_index
                });
            }
        }
    }

    ls->num_active_mesh_light_instances_ = (uint32_t)ls_ctx.active_mesh_light_instance_indices.size();
    ls->num_active_mesh_light_instance_clusters_ = num_active_mli_clusters;
    ls->max_active_mesh_light_instance_levels_ = max_active_mli_levels;

    ls->triangle_draw_count = (uint32_t)ls_ctx.triangle_draw_commands.size();
    ls->finalize_cluster_draw_count = (uint32_t)ls_ctx.finalize_cluster_draw_commands.size();
    ls->level_draw_counts.clear();
    ls->level_draw_counts.reserve(ls_ctx.level_draw_commands.size());
    for (auto & cmds : ls_ctx.level_draw_commands) {
        ls->level_draw_counts.push_back((uint32_t)cmds.size());
    }

    // Upload active mesh light instance index buffer
    ls->active_mesh_light_instance_index_buffer = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        (uint32_t)(std::max<size_t>(ls_ctx.active_mesh_light_instance_indices.size(), 1) * sizeof(uint32_t))
    );
    ls->active_mesh_light_instance_index_buffer->SetName("LightGrid_ActiveMeshLightInstanceIndexBuffer");
    if (!ls_ctx.active_mesh_light_instance_indices.empty()) {
        view->upload_context_.Add(
            ls->active_mesh_light_instance_index_buffer.Raw(),
            ls_ctx.active_mesh_light_instance_indices.data(),
            ls_ctx.active_mesh_light_instance_indices.size() * sizeof(uint32_t)
        );
    }
    view->upload_context_.AddExtraBarrier(ls->active_mesh_light_instance_index_buffer.Raw());

    // Upload active mesh light instance count
    view->upload_context_.Add(ls->active_mesh_light_instance_count.Raw(), &ls->num_active_mesh_light_instances_, sizeof(uint32_t));
    view->upload_context_.AddExtraBarrier(ls->active_mesh_light_instance_count.Raw());

    // Upload triangle precompute draw commands
    ls->precompute_triangle_draw_command_buffer = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kIndirect,
        (uint32_t)(std::max<size_t>(ls_ctx.triangle_draw_commands.size(), 1) * sizeof(RHIDrawIndirectCommand))
    );
    ls->precompute_triangle_draw_command_buffer->SetName("LightPrecomputationTriangleDrawCommandBuffer");
    if (!ls_ctx.triangle_draw_commands.empty()) {
        view->upload_context_.Add(
            ls->precompute_triangle_draw_command_buffer.Raw(),
            ls_ctx.triangle_draw_commands.data(),
            ls_ctx.triangle_draw_commands.size() * sizeof(RHIDrawIndirectCommand)
        );
    }
    view->upload_context_.AddExtraBarrier(ls->precompute_triangle_draw_command_buffer.Raw());

    // Upload finalize cluster draw commands
    ls->precompute_finalize_cluster_draw_command_buffer = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kIndirect,
        (uint32_t)(std::max<size_t>(ls_ctx.finalize_cluster_draw_commands.size(), 1) * sizeof(RHIDrawIndirectCommand))
    );
    ls->precompute_finalize_cluster_draw_command_buffer->SetName("LightPrecomputationFinalizeClusterDrawCommandBuffer");
    if (!ls_ctx.finalize_cluster_draw_commands.empty()) {
        view->upload_context_.Add(
            ls->precompute_finalize_cluster_draw_command_buffer.Raw(),
            ls_ctx.finalize_cluster_draw_commands.data(),
            ls_ctx.finalize_cluster_draw_commands.size() * sizeof(RHIDrawIndirectCommand)
        );
    }
    view->upload_context_.AddExtraBarrier(ls->precompute_finalize_cluster_draw_command_buffer.Raw());

    // Upload level draw commands
    ls->precompute_level_draw_command_buffers.clear();
    ls->precompute_level_draw_command_buffers.reserve(ls_ctx.level_draw_commands.size());
    for (uint32_t level_index = 0; level_index < (uint32_t)ls_ctx.level_draw_commands.size(); ++level_index) {
        auto & commands = ls_ctx.level_draw_commands[level_index];
        auto cmd_buffer = RDGBuffer::Create(
            RHIBufferUsageFlagBits::kIndirect,
            (uint32_t)(std::max<size_t>(commands.size(), 1) * sizeof(RHIDrawIndirectCommand))
        );
        cmd_buffer->SetName("LightPrecomputationLevelDrawCommandBuffer");
        if (!commands.empty()) {
            view->upload_context_.Add(
                cmd_buffer.Raw(),
                commands.data(),
                commands.size() * sizeof(RHIDrawIndirectCommand)
            );
        }
        view->upload_context_.AddExtraBarrier(cmd_buffer.Raw());
        ls->precompute_level_draw_command_buffers.push_back(std::move(cmd_buffer));
    }
}

void Renderer::Render_BuildLightStructure (RendererView * view, RenderGraphBuilder & builder) {
    RDGSectionGuard section(builder, "Render_BuildLightStructure");
    auto & lib = RDGShaderLibrary::Get();
    auto * ls = view->light_structure_.Raw();
    mi_check(ls, "Light structure data must exist before building the light structure.");
    auto persistent = view->persistent_data_->light_structure_persistent_data_;

    auto fill_common_params = [&](LightStructureParameters * params, uint32_t level_index) {
        FillParametersForLightStructure(view, params);
        auto * UB = builder.Allocate<LightStructureUB>();
        FillUniformBufferForLightStructure(view, UB);
        params->LightStructure_UB = UB;
        auto * level_ub = builder.Allocate<LightPrecomputationLevelUB>();
        level_ub->LevelIndex = level_index;
        level_ub->padding[0] = level_ub->padding[1] = level_ub->padding[2] = 0;
        params->LightPrecomputation_LevelUB = level_ub;

        params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
        params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
        params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
        params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
        params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
        params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
        params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
        params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());

        params->LCH_MeshLightTriangleBuffer = builder.Import(device_allocator_->GetMeshLightTriangleUberBufferArray()->GetRHI(0));
        params->LCH_MeshLightTriangleHashBuffer = builder.Import(device_allocator_->GetMeshLightTriangleUberBufferArray()->GetRHI(1));
        params->LCH_MeshLightTriangleBakedDataBuffer = builder.Import(device_allocator_->GetMeshLightTriangleUberBufferArray()->GetRHI(2));
        params->LCH_MeshLightClusterHeaderBuffer = builder.Import(device_allocator_->GetMeshLightClusterUberBufferArray()->GetRHI(0));
        params->LCH_MeshLightClusterNodeBuffer = builder.Import(device_allocator_->GetMeshLightClusterUberBufferArray()->GetRHI(1));
        params->LCH_MeshLightBuffer = builder.Import(device_allocator_->GetMeshLightUberBuffer()->GetRHI());
        params->LCH_MeshLightLevelHeaderBuffer = builder.Import(device_allocator_->GetMeshLightLevelHeaderUberBuffer()->GetRHI());
        params->LCH_MeshLightInstanceBuffer = builder.Import(device_allocator_->GetMeshLightInstanceUberBuffer()->GetRHI());
        params->LCH_MeshLightInstanceClusterHeaderBuffer = builder.Import(device_allocator_->GetMeshLightInstanceClusterUberBufferArray()->GetRHI(0));
        params->LCH_RWMeshLightInstanceClusterHeaderBuffer = builder.Import(device_allocator_->GetMeshLightInstanceClusterUberBufferArray()->GetRHI(0));
        params->LCH_MeshLightInstanceClusterNodeBuffer = builder.Import(device_allocator_->GetMeshLightInstanceClusterUberBufferArray()->GetRHI(1));
        params->LCH_RWMeshLightInstanceClusterNodeBuffer = builder.Import(device_allocator_->GetMeshLightInstanceClusterUberBufferArray()->GetRHI(1));
        params->LCH_MeshLightInstanceTriangleBuffer = builder.Import(device_allocator_->GetMeshLightInstanceTriangleUberBuffer()->GetRHI());
        params->LCH_RWMeshLightInstanceTriangleBuffer = builder.Import(device_allocator_->GetMeshLightInstanceTriangleUberBuffer()->GetRHI());
    };

    auto ini_macros = GetLightStructureShaderMacros();
    RDGShaderInitializationInfo ini;
    ini.optional_macros = ini_macros;

    auto * common_params = builder.Allocate<LightStructureParameters>();
    fill_common_params(common_params, 0);
    auto common_table = builder.Allocate<SharedParameterTableId>();

    {
        auto num_groups = DivideAndRoundUp(
            kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize,
            ClearLightStructureHistoryShader::kThreadGroupSize
        );
        if (persistent->need_reset_) {
            auto history_shader = lib.GetShader<ClearLightStructureHistoryShader>(ini);
            auto history_params = builder.Allocate<ClearLightStructureHistoryShader::Params>();
            FillParametersForLightStructure(view, history_params);
            history_params->LightStructure_UB = common_params->LightStructure_UB;
            Helpers::AddComputePass<ClearLightStructureHistoryShader>(
                builder, history_shader, history_params, num_groups
            );

            auto clear_grid_states_shader = lib.GetShader<ClearGridStatesShader>(ini);
            Helpers::AddComputePass<ClearGridStatesShader>(
                builder, clear_grid_states_shader, common_params, common_table, num_groups
            );

            persistent->need_reset_ = false;
        }

        Helpers::Clear(builder, ls->next_bloom_filter_buffer.Raw());
        Helpers::Clear(builder, ls->next_environment_visibility_buffer.Raw());
    }

    {
        auto shader = lib.GetShader<ClearCountersShader>(ini);
        auto num_groups = DivideAndRoundUp(kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize, ClearCountersShader::kThreadGroupSize);
        Helpers::AddComputePass(builder, shader, common_params, common_table, num_groups);
    }

    if (ls->num_active_mesh_light_instances_ == 0) {
        return;
    }

    {
        auto shader = lib.GetShader<PrecomputeTrianglesShader>(ini);
        builder.AddPass<PrecomputeTrianglesShader>({}, shader, common_params,
            [shader, params = common_params, cmd = ls->precompute_triangle_draw_command_buffer.Raw(), draw_count = ls->triangle_draw_count]
            (RDGPass * pass, RHICommandQueueGraphics & queue_inner) {
                if (draw_count == 0) {
                    return;
                }
                auto tid = RDGCommandHelper::CreateParameterTable(queue_inner, pass, shader, params);
                RDGCommandHelper::BeginGraphicsRender(queue_inner, shader, tid,
                    &PrecomputeTrianglesShader::GetShaderParamStructInfo()->render_pass_info_, params);
                queue_inner.DrawIndirect(cmd->GetRHI(), draw_count);
                RDGCommandHelper::EndGraphicsRender(queue_inner);
            }
        )->AddBufferH(ls->precompute_triangle_draw_command_buffer.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
    }

    for (int32_t level_dispatch_index = (int32_t)ls->precompute_level_draw_command_buffers.size() - 1; level_dispatch_index >= 0; --level_dispatch_index) {
        auto const draw_count = ls->level_draw_counts[level_dispatch_index];
        auto * params = builder.Allocate<LightStructureParameters>();
        fill_common_params(params, (uint32_t)level_dispatch_index * kLightPrecomputationLevelsPerDispatch);
        auto shader = lib.GetShader<PrecomputeLevelShader>(ini);
        builder.AddPass<PrecomputeLevelShader>({}, shader, params,
            [shader, params, cmd = ls->precompute_level_draw_command_buffers[level_dispatch_index].Raw(), draw_count]
            (RDGPass * pass, RHICommandQueueGraphics & queue_inner) {
                if (draw_count == 0) {
                    return;
                }
                auto tid = RDGCommandHelper::CreateParameterTable(queue_inner, pass, shader, params);
                RDGCommandHelper::BeginGraphicsRender(queue_inner, shader, tid,
                    &PrecomputeLevelShader::GetShaderParamStructInfo()->render_pass_info_, params);
                queue_inner.DrawIndirect(cmd->GetRHI(), draw_count);
                RDGCommandHelper::EndGraphicsRender(queue_inner);
            }
        )->AddBufferH(ls->precompute_level_draw_command_buffers[level_dispatch_index].Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
    }

    {
        auto shader = lib.GetShader<FinalizeMLIClustersShader>(ini);
        builder.AddPass<FinalizeMLIClustersShader>({}, shader, common_params,
            [shader, params = common_params, cmd = ls->precompute_finalize_cluster_draw_command_buffer.Raw(), draw_count = ls->finalize_cluster_draw_count]
            (RDGPass * pass, RHICommandQueueGraphics & queue_inner) {
                if (draw_count == 0) {
                    return;
                }
                auto tid = RDGCommandHelper::CreateParameterTable(queue_inner, pass, shader, params);
                RDGCommandHelper::BeginGraphicsRender(queue_inner, shader, tid,
                    &FinalizeMLIClustersShader::GetShaderParamStructInfo()->render_pass_info_, params);
                queue_inner.DrawIndirect(cmd->GetRHI(), draw_count);
                RDGCommandHelper::EndGraphicsRender(queue_inner);
            }
        )->AddBufferH(ls->precompute_finalize_cluster_draw_command_buffer.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
    }

    {
        auto shader = lib.GetShader<GatherActiveGridsShader>(ini);
        auto num_groups = DivideAndRoundUp(kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize, RHI::Get().GetDeviceProperties().wave_size * 8u);
        Helpers::AddComputePass(builder, shader, common_params, common_table, num_groups);
    }

    {
        auto shader = lib.GetShader<ClipActiveGridsShader>(ini);
        Helpers::AddComputePass(builder, shader, common_params, common_table);
    }

    {
        auto shader = lib.GetShader<InjectLightsShader>(ini);
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, ls->active_grid_count.Raw(), RHI::Get().GetDeviceProperties().wave_size);
        Helpers::AddComputeIndirectPass(builder, shader, common_params, common_table, cmd.Raw());
    }
}

void Renderer::Render_UpdateLightStructureHistory(RendererView *view, RenderGraphBuilder &builder) {
    auto & lib = RDGShaderLibrary::Get();
    auto num_light_grids = kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize;
    auto ini_macros = GetLightStructureShaderMacros();
    RDGShaderInitializationInfo ini;
    ini.optional_macros = ini_macros;
    auto shader = lib.GetShader<UpdateLightStructureHistoryShader>(ini);
    auto params = builder.Allocate<UpdateLightStructureHistoryShader::Params>();
    FillParametersForLightStructure(view, params);
    auto UB = builder.Allocate<LightStructureUB>();
    FillUniformBufferForLightStructure(view, UB);
    params->LightStructure_UB = UB;
    auto num_groups = DivideAndRoundUp(num_light_grids, UpdateLightStructureHistoryShader::kThreadGroupSize);
    Helpers::AddComputePass<UpdateLightStructureHistoryShader>(
        builder, shader, params, num_groups
    );
}

MI_NAMESPACE_END
