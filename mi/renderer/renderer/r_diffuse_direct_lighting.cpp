/*
 * Created: 2025/7/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <rdg/rdg_shader.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_cvar.h>
#include <renderer/mi_renderer.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_scene.h>
#include "../shaders/shared/SharedLight.hlsl"
#include "../shaders/shared/SharedDebug.hlsl"
#include "r_view_common.h"
#include "r_persistent.h"
#include "r_light_structure.h"
#include "r_direct_lighting.h"
#include "r_diffuse_direct_lighting.h"
MI_NAMESPACE_BEGIN
// Some CVars are exposed through r_diffuse_direct_lighting.h
static CVar<bool> CVar_DebugOutputTransmittanceRaysForMesh(
    "r.direct_lighting.debug.output_transmittance_rays_for_mesh",
    "Write the transmittance of shadow rays to the output for the specified mesh index. 0 to disable.",
    false
);

void DiffuseDirectLightingData::Allocate(RenderGraphBuilder &builder, RendererView *view) {
    radiance = builder.CreateTexture2D(
        view->film_width_, view->film_height_,
        PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransfer
    );
    radiance->SetName("DiffuseDirectLightingTexture");
}

BEGIN_SHADER_PARAMETERS(DirectLightingShaderParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(DebugCommonShaderParameters, Debug)
    SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)
    SHADER_UNIFORM_BUFFER(DirectLightingUB, DirectLighting_UB)
    SHADER_UNIFORM_BUFFER(HybridTracingUB, HybridTracing_UB)
    SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointBorder1Sampler)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightBuffer)

    // Light grid
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_PrecomputedActiveLightBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ActiveLightListCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ActiveLightListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ListAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ListActiveLightListIndexBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListCdfBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListLengthBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_EnvironmentVisibilityHistoryBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_BloomFilterBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_NextBloomFilterBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_NextEnvironmentVisibilityBuffer)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceCount)
    // SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceListAllocator)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceOriginScreenCoordBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadowRayToTraceTMaxBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ShadowRayToTraceTMaxBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ShadowRayToTraceTransmittanceBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWShadowRayToTraceSampledLightIndexBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_DepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_NormalTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_HiZBuffer)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_HistoryDepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, G_FlagsTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, OrFlagsTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDiffuseDirectLightingTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDirectLightingRayIndexTexture)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDirectLightingRadianceEstimateTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, DirectLightingRadianceEstimateTexture)

    // For debugging purposes
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRaysCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayOrigins)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayDirections)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWDebugTracedRayStates)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(DirectLightingShaderParameters)

class DiffuseDirectLightingShader : public RDGShader {
public:
    static constexpr uint32_t kThreadGroupSize = 128;
    static std::vector<std::string> GetShaderDefaultMacros() {
        return {
            "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
            "THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)
        };
    }
    static std::vector<std::string> GetShaderOptionalMacros() {
        return GetLightStructureShaderMacros();
    }
    using RDGShader::RDGShader;
};

class ClearLightGridShader : public DiffuseDirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER(DiffuseDirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClearLightGridShader, "mi/renderer/shaders/DirectLighting.hlsl", "ClearLightGrid");

class PrecomputeLightsShader : public DiffuseDirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER(DiffuseDirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(PrecomputeLightsShader, "mi/renderer/shaders/DirectLighting.hlsl", "PrecomputeLights");

class InjectLightsShader : public DiffuseDirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER(DiffuseDirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(InjectLightsShader, "mi/renderer/shaders/DirectLighting.hlsl", "InjectLights");

class SpawnLightSamplesShader : public DiffuseDirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER(DiffuseDirectLightingShader)
    constexpr static uint32_t kTileSize = 8;
    static std::vector<std::string> GetShaderDefaultMacros() {
        auto ret = DiffuseDirectLightingShader::GetShaderDefaultMacros();
        ret.push_back(
            "TILE_SIZE=" + std::to_string(kTileSize)
        );
        return ret;
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(SpawnLightSamplesShader, "mi/renderer/shaders/DirectLighting.hlsl", "SpawnLightSamples");

class ScreenSpaceTraceForDirectLightingShader : public DiffuseDirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER(DiffuseDirectLightingShader)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ScreenSpaceTraceForDirectLightingShader, "mi/renderer/shaders/DirectLighting.hlsl", "ScreenSpaceTraceForDirectLighting");

class RenderDiffuseDirectLightingShader : public DiffuseDirectLightingShader {
public:
    RDG_SHADER_USE_PARAMETERS(DirectLightingShaderParameters)
    DECLARE_SHADER(DiffuseDirectLightingShader)

    static std::vector<std::string> GetShaderOptionalMacros() {
        auto macros = DiffuseDirectLightingShader::GetShaderOptionalMacros();
        macros.push_back("DEBUG_OUTPUT_TRACED_RAY");
        return macros;
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(RenderDiffuseDirectLightingShader, "mi/renderer/shaders/DirectLighting.hlsl", "RenderDiffuseDirectLighting");

void Renderer::Render_ComputeDiffuseDirectLighting(RendererView *view, RenderGraphBuilder &builder) {
    RDGSectionGuard section(builder, "Render_ComputeDirectDiffuseLighting");

    auto & lib = RDGShaderLibrary::Get();
    auto ini_macros = GetLightStructureShaderMacros();
    auto ini = RDGShaderInitializationInfo{};
    ini.optional_macros = ini_macros;

    auto params = builder.Allocate<DirectLightingShaderParameters>();
    auto light_buffer = builder.Import(device_allocator_->GetAreaLightsUberBuffer()->GetRHI());
    auto max_num_lights = device_allocator_->GetAreaLightsUberBuffer()->GetAllocationLimitByteOffset() / sizeof(RawLight);
    auto num_light_grids = kLightGridNumCascades * kLightGridSize * kLightGridSize * kLightGridSize;

    uint32_t num_screen_pixels = view->film_width_ * view->film_height_;

    auto ray_to_trace_count = builder.CreateBuffer<uint32_t>();
    ray_to_trace_count->SetName("RayToTraceCount");

    auto ray_to_trace_list_allocator = builder.CreateBuffer<uint32_t>();
    ray_to_trace_list_allocator->SetName("RayToTraceListAllocator");

    auto ray_to_trace_list = builder.CreateBuffer<uint32_t>(num_screen_pixels);
    ray_to_trace_list->SetName("RayToTraceList");
    auto ray_to_trace_direction = builder.CreateBuffer<glm::vec3>(num_screen_pixels);
    ray_to_trace_direction->SetName("RayToTraceDirection");
    auto ray_to_trace_state = builder.CreateBuffer<uint32_t>(num_screen_pixels);
    ray_to_trace_state->SetName("RayToTraceState");
    auto ray_to_trace_origin_screen_coords = builder.CreateBuffer<glm::uvec2>(num_screen_pixels);
    ray_to_trace_origin_screen_coords->SetName("RayToTraceOriginScreenCoord");

    auto direct_lighting_radiance_estimate_texture = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR16G16B16A16_FLOAT
    );
    direct_lighting_radiance_estimate_texture->SetName("DirectLightingRadianceEstimateTexture");
    auto direct_lighting_ray_index_texture = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR32_UINT
    );
    direct_lighting_ray_index_texture->SetName("DirectLightingRayIndexTexture");

    auto shadow_ray_to_trace_tmax = builder.CreateBuffer<float>(num_screen_pixels);
    shadow_ray_to_trace_tmax->SetName("ShadowRayToTraceTMaxBuffer");

    auto shadow_ray_to_trace_transmittance = builder.CreateBuffer<float>(num_screen_pixels);
    shadow_ray_to_trace_transmittance->SetName("ShadowRayToTraceTransmittanceBuffer");

    auto shadow_ray_to_trace_sampled_light_index = builder.CreateBuffer<uint32_t>(num_screen_pixels);
    shadow_ray_to_trace_sampled_light_index->SetName("ShadowRayToTraceSampledLightIndexBuffer");
    {
        params->View = view->view_common_params_;

        auto L_UB = builder.Allocate<LightStructureUB>();
        FillUniformBufferForLightStructure(view, L_UB);
        params->LightStructure_UB = L_UB;

        auto DI_UB = builder.Allocate<DirectLightingUB>();
        FillUniformBufferForDirectLighting(view, DI_UB);
        params->DirectLighting_UB = DI_UB;
        auto HT_UB = builder.Allocate<HybridTracingUB>();
        FillUniformBufferForHybridTracing(view, HT_UB);
        params->HybridTracing_UB = HT_UB;
        params->LightBuffer = light_buffer;

        FillParametersForLightStructure(view, params);

        params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
        params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
        params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
        params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
        params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
        params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
        params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
        params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());

        params->RWRayToTraceCount = ray_to_trace_count.Raw();
        params->RayToTraceCount = ray_to_trace_count.Raw();
        params->RWRayToTraceListAllocator = ray_to_trace_list_allocator.Raw();
        params->RWRayToTraceListBuffer = ray_to_trace_list.Raw();
        params->RWRayToTraceDirectionBuffer = ray_to_trace_direction.Raw();
        params->RWRayToTraceStateBuffer = ray_to_trace_state.Raw();
        params->RWRayToTraceOriginScreenCoordBuffer = ray_to_trace_origin_screen_coords.Raw();

        params->RWShadowRayToTraceTMaxBuffer = shadow_ray_to_trace_tmax.Raw();
        params->ShadowRayToTraceTMaxBuffer = shadow_ray_to_trace_tmax.Raw();
        params->ShadowRayToTraceTransmittanceBuffer = shadow_ray_to_trace_transmittance.Raw();
        params->RWShadowRayToTraceSampledLightIndexBuffer = shadow_ray_to_trace_sampled_light_index.Raw();

        params->G_DepthTexture = view->G_depth_.Raw();
        params->G_NormalTexture = view->G_normal_.Raw();
        params->G_HiZBuffer = view->hzb_.Raw();
        params->G_HistoryDepthTexture = view->persistent_data_->prev_G_depth.Raw();
        params->G_FlagsTexture = view->G_flags_.Raw();
        params->OrFlagsTexture = view->or_flags_.Raw();
        params->RWDiffuseDirectLightingTexture = view->diffuse_direct_lighting_->radiance.Raw();
        params->RWDirectLightingRayIndexTexture = direct_lighting_ray_index_texture.Raw();
        params->RWDirectLightingRadianceEstimateTexture = direct_lighting_radiance_estimate_texture.Raw();
        params->DirectLightingRadianceEstimateTexture = direct_lighting_radiance_estimate_texture.Raw();

        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
        params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
        params->PointWrapSampler  = RHI::Get().GetGlobalSamplers().point_wrap;
        // Specially prepared sampler for SSRT
        params->PointBorder1Sampler = RHI::Get().GetGlobalSamplers().point_border_1;

        if (CVar_DebugOutputTransmittanceRaysForMesh.Get() && !view->debug_buffers_.traced_ray_count) {
            view->debug_buffers_.CreateTracedRayBuffers(builder, 16);
            params->RWDebugTracedRaysCount = view->debug_buffers_.traced_ray_count.Raw();
            params->RWDebugTracedRayOrigins = view->debug_buffers_.traced_ray_origins.Raw();
            params->RWDebugTracedRayDirections = view->debug_buffers_.traced_ray_directions.Raw();
            params->RWDebugTracedRayStates = view->debug_buffers_.traced_ray_states.Raw();
        } else {
            params->RWDebugTracedRaysCount = nullptr;
            params->RWDebugTracedRayOrigins = nullptr;
            params->RWDebugTracedRayDirections = nullptr;
            params->RWDebugTracedRayStates = nullptr;
        }

        params->Debug = view->debug_common_params_;
    }
    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
    // Clear counters
    {
        auto shader = lib.GetShader<ClearLightGridShader>(ini);
        auto num_groups = DivideAndRoundUp(num_light_grids, DiffuseDirectLightingShader::kThreadGroupSize);
        Helpers::AddComputePass<ClearLightGridShader>(
            builder, shader, params, num_groups
        );
    }
    // Precompute lights
    {
        auto shader = lib.GetShader<PrecomputeLightsShader>(ini);
        auto num_groups = DivideAndRoundUp(max_num_lights, DiffuseDirectLightingShader::kThreadGroupSize);
        Helpers::AddComputePass<PrecomputeLightsShader>(
            builder, shader, params, (uint32_t)num_groups
        );
    }
    {
        auto shader = lib.GetShader<InjectLightsShader>(ini);
        auto num_groups = DivideAndRoundUp(num_light_grids, wave_size);
        Helpers::AddComputePass<InjectLightsShader>(
            builder, shader, params, num_groups
        );
    }
    {
        auto shader = lib.GetShader<SpawnLightSamplesShader>(ini);
        auto tile_size = SpawnLightSamplesShader::kTileSize;
        auto num_groups_x = DivideAndRoundUp(view->film_width_, tile_size);
        auto num_groups_y = DivideAndRoundUp(view->film_height_, tile_size);
        Helpers::AddComputePass<SpawnLightSamplesShader>(
            builder, shader, params, num_groups_x, num_groups_y
        );
    }
    auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, ray_to_trace_count.Raw(), wave_size);
    {
        auto shader = lib.GetShader<ScreenSpaceTraceForDirectLightingShader>(ini);
        Helpers::AddComputeIndirectPass<ScreenSpaceTraceForDirectLightingShader>(
            builder, shader, params, cmd.Raw()
        );
    }

    // Continue the rays that does not hit anything or went through volumes using HWRT
    {
        // HWRT
        Render_HardwareTransmittanceRayTracing(
            view, builder,
            ray_to_trace_list_allocator.Raw(),
            ray_to_trace_list.Raw(),
            ray_to_trace_direction.Raw(),
            ray_to_trace_state.Raw(),
            ray_to_trace_origin_screen_coords.Raw(),
            nullptr,
            shadow_ray_to_trace_tmax.Raw(),
            shadow_ray_to_trace_transmittance.Raw()
        );
    }
    {
        auto ini_t = ini;
        if (CVar_DebugOutputTransmittanceRaysForMesh.Get()) {
            // Enable debug output of traced rays
            ini_t.optional_macros.push_back("DEBUG_OUTPUT_TRACED_RAY");
        }
        auto shader = lib.GetShader<RenderDiffuseDirectLightingShader>(ini_t);
        Helpers::Clear(builder, view->diffuse_direct_lighting_->radiance.Raw());
        Helpers::AddComputeIndirectPass<RenderDiffuseDirectLightingShader>(
            builder, shader, params, cmd.Raw()
        );
    }


}


MI_NAMESPACE_END