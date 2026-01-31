/*
* Created: 2025/11/9
 * Author: Exploring Air Joe
 * See LICENSE for licensing.
 */

#include <rdg/rdg_shader.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_cvar.h>
#include <renderer/mi_renderer.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_scene.h>
#include <renderer/mi_buffer_heap.h>
#include <renderer/r_geometry_buffer.h>

#include "r_view_common.h"
#include "r_light_structure.h"
#include "r_direct_lighting.h"
#include "r_volume_grid_direct_lighting.h"

MI_NAMESPACE_BEGIN
    void VolumeGridDirectLightingData::Allocate(RenderGraphBuilder &builder, RendererView *view) {
    radiance = builder.CreateTexture2D(
        view->film_width_, view->film_height_,
        PixelFormatType::kR16G16B16A16_FLOAT
    );
    radiance->SetName("VolumeGridDirectLightingTexture");
}

BEGIN_SHADER_PARAMETERS(VolumeGridDirectLightingShaderParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)
    SHADER_UNIFORM_BUFFER(DirectLightingUB, DirectLighting_UB)
    SHADER_UNIFORM_BUFFER(HybridTracingUB, HybridTracing_UB)
    SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)

    // Light grid
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_PrecomputedActiveLightBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ActiveLightListCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ActiveLightListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_ListActiveLightListIndexBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListCdfBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_GridLightListLengthBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_EnvironmentVisibilityHistoryBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_BloomFilterBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_NextBloomFilterBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, LightGrid_NextEnvironmentVisibilityBuffer)

    // Renderable Instance
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumeGridHeaderBuffer)

    // G_Buffer
    SHADER_RESOURCE_PARAMETER(Texture2D, G_DepthTexture)

    // Transmittance Ray Tracing
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeGridTransmittanceRayToTraceCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeGridTransmittanceRayToTraceDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeGridTransmittanceRayToTraceStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeGridTransmittanceRayToTraceOriginBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeGridTransmittanceRayToTraceTMaxBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeGridTransmittanceRayToTraceSampledLightIndexBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeGridTransmittanceRayToTraceTransmittanceBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeGridTransmittanceRayToTracePixelIndexBuffer)

    // Intermediate variable
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeGridRadianceEstimateTexture)

    // Direct Lighting Result
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeGridSumTransmittanceTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeGridDirectLightingTexture)

END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(VolumeGridDirectLightingShaderParameters)

namespace VolumeGridDirectLightingShaders {
    class VolumeGridDirectLightingShader : public RDGShader {
    public:
        static constexpr uint32_t kThreadGroupSize = 128;
        static std::vector<std::string> GetShaderDefaultMacros() {
            return {
                "WAVE_SIZE=" + std::to_string(RHI::Get().GetDeviceProperties().wave_size),
                "THREAD_GROUP_SIZE" + std::to_string(kThreadGroupSize)
            };
        }
        static std::vector<std::string> GetShaderOptionalMacros() {
            return GetLightStructureShaderMacros();
        }
        using RDGShader::RDGShader;
    };

    class VolumeGridDirectLightingClearCounters : public VolumeGridDirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeGridDirectLightingShaderParameters)
        DECLARE_SHADER(VolumeGridDirectLightingShader)
    };
    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        VolumeGridDirectLightingClearCounters,
        "mi/renderer/shaders/DirectLighting.hlsl", "VolumeGridDirectLightingClearCounters"
    );

    class VolumeGridDirectLightingSpawnLightSamplesShader : public VolumeGridDirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeGridDirectLightingShaderParameters)
        DECLARE_SHADER(VolumeGridDirectLightingShader)
        constexpr static uint32_t kTileSize = 8;
        static std::vector<std::string> GetShaderDefaultMacros() {
            auto ret = VolumeGridDirectLightingShader::GetShaderDefaultMacros();
            ret.push_back(
                "TILE_SIZE=" + std::to_string(kTileSize)
            );
            return ret;
        }
    };
    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        VolumeGridDirectLightingSpawnLightSamplesShader,
        "mi/renderer/shaders/DirectLighting.hlsl", "VolumeGridDirectLightingSpawnLightSamples"
    );

    class RenderVolumeGridDirectLightingShader : public VolumeGridDirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumeGridDirectLightingShaderParameters)
        DECLARE_SHADER(VolumeGridDirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        RenderVolumeGridDirectLightingShader,
        "mi/renderer/shaders/DirectLighting.hlsl", "RenderVolumeGridDirectLighting"
    );
}

void Renderer::Render_ComputeVolumeGridDirectLighting(RendererView *view, RenderGraphBuilder &builder) {
    using namespace VolumeGridDirectLightingShaders;
    RDGSectionGuard section(builder, "Render_ComputeVolumeGridDirectLighting");
    auto & lib = RDGShaderLibrary::Get();
    auto ini_macros = GetLightStructureShaderMacros();
    auto ini = RDGShaderInitializationInfo{};
    ini.optional_macros = ini_macros;

    auto volume_grid_params = builder.Allocate<VolumeGridDirectLightingShaderParameters>();
    volume_grid_params->View = view->view_common_params_;
    auto LightStructure_UB = builder.Allocate<LightStructureUB>();
    FillUniformBufferForLightStructure(view, LightStructure_UB);
    volume_grid_params->LightStructure_UB = LightStructure_UB;
    auto DirectLighting_UB = builder.Allocate<DirectLightingUB>();
    FillUniformBufferForDirectLighting(view, DirectLighting_UB);
    volume_grid_params->DirectLighting_UB = DirectLighting_UB;
    auto HybridTracing_UB = builder.Allocate<HybridTracingUB>();
    FillUniformBufferForHybridTracing(view, HybridTracing_UB);
    volume_grid_params->HybridTracing_UB = HybridTracing_UB;

    volume_grid_params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    volume_grid_params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    volume_grid_params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;

    volume_grid_params->LightBuffer = builder.Import(device_allocator_->GetAreaLightsUberBuffer()->GetRHI());
    FillParametersForLightStructure(view, volume_grid_params);

    // Renderable Instance
    volume_grid_params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    volume_grid_params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    volume_grid_params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
    volume_grid_params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    volume_grid_params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    volume_grid_params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    volume_grid_params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    volume_grid_params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
    volume_grid_params->VolumeGridHeaderBuffer = builder.Import(device_allocator_->GetVolumeGridHeaderBuffer());

    // G_Buffer
    volume_grid_params->G_DepthTexture = view->g_buffer_->G_depth_.Raw();

    // Transmittance Ray to Trace
    auto num_screen_pixels = view->film_width_ * view->film_height_;
    auto volume_grid_transmittance_ray_to_trace_count = builder.CreateBuffer<uint32_t>();
    volume_grid_transmittance_ray_to_trace_count->SetName("VolumeGridTransmittanceRayToTraceCount");
    auto volume_grid_transmittance_ray_to_trace_direction = builder.CreateBuffer<glm::vec3>(num_screen_pixels);
    volume_grid_transmittance_ray_to_trace_direction->SetName("VolumeGridTransmittanceRayToTraceDirection");
    auto volume_grid_transmittance_ray_to_trace_origin = builder.CreateBuffer<glm::vec3>(num_screen_pixels);
    volume_grid_transmittance_ray_to_trace_origin->SetName("VolumeGridTransmittanceRayToTraceOrigin");
    auto volume_grid_transmittance_ray_to_trace_state = builder.CreateBuffer<uint32_t>(num_screen_pixels);
    volume_grid_transmittance_ray_to_trace_state->SetName("VolumeGridTransmittanceRayToTraceState");
    auto volume_grid_transmittance_ray_to_trace_tmax = builder.CreateBuffer<float>(num_screen_pixels);
    volume_grid_transmittance_ray_to_trace_tmax->SetName("VolumeGridTransmittanceRayToTraceTMax");
    auto volume_grid_transmittance_ray_to_trace_sampled_light_index = builder.CreateBuffer<uint32_t>(num_screen_pixels);
    volume_grid_transmittance_ray_to_trace_sampled_light_index->SetName("VolumeGridTransmittanceRayToTraceSampledLightIndex");
    auto volume_grid_transmittance_ray_to_trace_transmittance = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(float)
    );
    volume_grid_transmittance_ray_to_trace_transmittance->SetName("VolumeGridTransmittanceRayToTraceTransmittance");
    auto volume_grid_transmittance_ray_to_trace_pixel_index = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(uint32_t)
    );
    volume_grid_transmittance_ray_to_trace_pixel_index->SetName("VolumeGridTransmittanceRayToTracePixelIndex");
    volume_grid_params->RWVolumeGridTransmittanceRayToTraceCount = volume_grid_transmittance_ray_to_trace_count.Raw();
    volume_grid_params->RWVolumeGridTransmittanceRayToTraceDirectionBuffer = volume_grid_transmittance_ray_to_trace_direction.Raw();
    volume_grid_params->RWVolumeGridTransmittanceRayToTraceStateBuffer = volume_grid_transmittance_ray_to_trace_state.Raw();
    volume_grid_params->RWVolumeGridTransmittanceRayToTraceOriginBuffer = volume_grid_transmittance_ray_to_trace_origin.Raw();
    volume_grid_params->RWVolumeGridTransmittanceRayToTraceTMaxBuffer = volume_grid_transmittance_ray_to_trace_tmax.Raw();
    volume_grid_params->RWVolumeGridTransmittanceRayToTraceSampledLightIndexBuffer = volume_grid_transmittance_ray_to_trace_sampled_light_index.Raw();
    volume_grid_params->RWVolumeGridTransmittanceRayToTraceTransmittanceBuffer = volume_grid_transmittance_ray_to_trace_transmittance.Raw();
    volume_grid_params->RWVolumeGridTransmittanceRayToTracePixelIndexBuffer = volume_grid_transmittance_ray_to_trace_pixel_index.Raw();

    auto volume_grid_radiance_estimate_texture = builder.CreateTexture(
        RHITextureDesc{RHITextureType::k2D, RHITextureDimensions {view->film_width_, view->film_height_, 1},
            1, 1, PixelFormatType::kR16G16B16A16_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource}
    );
    volume_grid_params->RWVolumeGridRadianceEstimateTexture = volume_grid_radiance_estimate_texture.Raw();

    volume_grid_params->RWVolumeGridSumTransmittanceTexture = view->g_buffer_->G_transmittance_.Raw();
    volume_grid_params->RWVolumeGridDirectLightingTexture = view->volume_grid_direct_lighting_->radiance.Raw();

    // Volume Grid Direct Lighting
    {
        auto shader = lib.GetShader<VolumeGridDirectLightingClearCounters>(ini);
        Helpers::AddComputePass<VolumeGridDirectLightingClearCounters>(builder, shader, volume_grid_params);
    }
    {
        auto shader = lib.GetShader<VolumeGridDirectLightingSpawnLightSamplesShader>(ini);
        auto tile_size = VolumeGridDirectLightingSpawnLightSamplesShader::kTileSize;
        auto num_group_x = DivideAndRoundUp(view->film_width_, tile_size);
        auto num_group_y = DivideAndRoundUp(view->film_height_, tile_size);
        Helpers::AddComputePass<VolumeGridDirectLightingSpawnLightSamplesShader>(
            builder, shader, volume_grid_params, num_group_x, num_group_y
        );
    }

    {
        // HWRT
        Render_HardwareTransmittanceRayTracing(
            view, builder,
            volume_grid_transmittance_ray_to_trace_count.Raw(),
            nullptr,
            volume_grid_transmittance_ray_to_trace_direction.Raw(),
            volume_grid_transmittance_ray_to_trace_state.Raw(),
            nullptr,
            volume_grid_transmittance_ray_to_trace_origin.Raw(),
            volume_grid_transmittance_ray_to_trace_tmax.Raw(),
            volume_grid_transmittance_ray_to_trace_transmittance.Raw()
        );
    }

    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
    auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, volume_grid_transmittance_ray_to_trace_count.Raw(), wave_size);
    {
        auto shader = lib.GetShader<RenderVolumeGridDirectLightingShader>(ini);
        Helpers::AddComputeIndirectPass<RenderVolumeGridDirectLightingShader>(
            builder, shader, volume_grid_params, cmd.Raw()
        );
    }
}


MI_NAMESPACE_END
