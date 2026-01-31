/*
 * Created: 2025/11/9
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "r_volume_direct_lighting.h"

#include <rdg/rdg_shader.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_cvar.h>
#include <renderer/mi_renderer.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_scene.h>

#include "r_view_common.h"
#include "r_direct_lighting.h"
#include "../shaders/shared/SharedLight.hlsl"
#include "../shaders/shared/SharedDebug.hlsl"
#include "r_persistent.h"
#include "r_light_structure.h"
#include "r_volume_primitives.h"
#include "r_volume_direct_lighting.h"

#include "../include/renderer/r_geometry_buffer.h"
#include "renderer/mi_buffer_heap.h"

MI_NAMESPACE_BEGIN
    void VolumeDirectLightingData::Allocate(RenderGraphBuilder &builder, RendererView *view) {
    radiance = builder.CreateTexture2D(
        view->film_width_, view->film_height_,
        PixelFormatType::kR16G16B16A16_FLOAT
    );
    radiance->SetName("VolumeDirectLightingTexture");
}

BEGIN_SHADER_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(LightStructureUB, LightStructure_UB)
    SHADER_UNIFORM_BUFFER(DirectLightingUB, DirectLighting_UB)
    SHADER_UNIFORM_BUFFER(HybridTracingUB, HybridTracing_UB)
    SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightBuffer)

    // Light grid
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

    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeSampleColorTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeSampleLinearDepthTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeSampleTransmittanceAndPdfTexture)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeDirectLightingRadianceEstimateTexture)
    SHADER_RESOURCE_PARAMETER(Texture2D, VolumeDirectLightingRadianceEstimateTexture)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_DepthTexture)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceCount)
    // SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceDirectionBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceStateBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceOriginBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceTMaxBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTraceSampledLightIndexBuffer)

    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWVolumeRayToTracePixelIndexBuffer)

    SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumeRayToTraceTransmittanceBuffer)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeDirectLightingTexture)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)

namespace VolumeDirectLightingShaders {
    class VolumeDirectLightingShader : public RDGShader {
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

    class VolumeDirectLightingClearCounters : public VolumeDirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)
        DECLARE_SHADER(VolumeDirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        VolumeDirectLightingClearCounters,
        "mi/renderer/shaders/DirectLighting.hlsl", "VolumeDirectLightingClearCounters");

    class VolumeDirectLightingSpawnLightSamplesShader : public VolumeDirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)
        DECLARE_SHADER(VolumeDirectLightingShader)
        constexpr static uint32_t kTileSize = 8;
        static std::vector<std::string> GetShaderDefaultMacros() {
            auto ret = VolumeDirectLightingShader::GetShaderDefaultMacros();
            ret.push_back(
                "TILE_SIZE=" + std::to_string(kTileSize)
            );
            return ret;
        }
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        VolumeDirectLightingSpawnLightSamplesShader,
        "mi/renderer/shaders/DirectLighting.hlsl", "VolumeDirectLightingSpawnLightSamples");

    class RenderVolumeDirectLightingShader : public VolumeDirectLightingShader {
    public:
        RDG_SHADER_USE_PARAMETERS(VolumePrimitivesDirectLightingShaderParameters)
        DECLARE_SHADER(VolumeDirectLightingShader)
    };

    IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(
        RenderVolumeDirectLightingShader,
        "mi/renderer/shaders/DirectLighting.hlsl", "RenderVolumeDirectLighting");
}

void Renderer::Render_ComputeVolumeDirectLighting(RendererView *view, RenderGraphBuilder &builder) {
    using namespace VolumeDirectLightingShaders;
    RDGSectionGuard section(builder, "Render_ComputeVolumeDirectLighting");
    auto & lib = RDGShaderLibrary::Get();
    auto ini_macros = GetLightStructureShaderMacros();
    auto ini = RDGShaderInitializationInfo{};
    ini.optional_macros = ini_macros;

    auto volprims_params = builder.Allocate<VolumePrimitivesDirectLightingShaderParameters>();
    volprims_params->View = view->view_common_params_;
    auto L_UB = builder.Allocate<LightStructureUB>();
    FillUniformBufferForLightStructure(view, L_UB);
    volprims_params->LightStructure_UB = L_UB;
    auto DI_UB = builder.Allocate<DirectLightingUB>();
    FillUniformBufferForDirectLighting(view, DI_UB);
    volprims_params->DirectLighting_UB = DI_UB;
    auto HT_UB = builder.Allocate<HybridTracingUB>();
    FillUniformBufferForHybridTracing(view, HT_UB);
    volprims_params->HybridTracing_UB = HT_UB;

    volprims_params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    volprims_params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    volprims_params->PointWrapSampler  = RHI::Get().GetGlobalSamplers().point_wrap;

    volprims_params->LightBuffer = builder.Import(device_allocator_->GetAreaLightsUberBuffer()->GetRHI());
    auto num_screen_pixels = view->film_width_ * view->film_height_;
    auto volume_ray_to_trace_count = builder.CreateBuffer<uint32_t>();
    volume_ray_to_trace_count->SetName("VolumeRayToTraceCount");
    auto volume_ray_to_trace_direction = builder.CreateBuffer<glm::vec3>(num_screen_pixels);
    auto volume_ray_to_trace_origin = builder.CreateBuffer<glm::vec3>(num_screen_pixels);
    auto volume_ray_to_trace_state = builder.CreateBuffer<uint32_t>(num_screen_pixels);
    auto volume_ray_to_trace_tmax = builder.CreateBuffer<float>(num_screen_pixels);
    auto volume_ray_to_trace_sampled_light_index = builder.CreateBuffer<uint32_t>(num_screen_pixels);
    FillParametersForLightStructure(view, volprims_params);

    volprims_params->VolumeSampleColorTexture       = view->volume_primitives_->volume_sample_color_.Raw();
    volprims_params->VolumeSampleLinearDepthTexture = view->volume_primitives_->volume_sample_linear_depth_.Raw();
    volprims_params->VolumeSampleTransmittanceAndPdfTexture = view->volume_primitives_->volume_sample_transmittance_and_pdf_.Raw();
    auto volume_di_radiance_estimate_texture = builder.CreateTexture(
        RHITextureDesc{RHITextureType::k2D, RHITextureDimensions {view->film_width_, view->film_height_, 1},
            1, 1, PixelFormatType::kR16G16B16A16_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource}
    );

    volprims_params->RWVolumeDirectLightingRadianceEstimateTexture = volume_di_radiance_estimate_texture.Raw();
    volprims_params->VolumeDirectLightingRadianceEstimateTexture = volume_di_radiance_estimate_texture.Raw();

    volprims_params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    volprims_params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    volprims_params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
    volprims_params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    volprims_params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    volprims_params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    volprims_params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    volprims_params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());

    volprims_params->G_DepthTexture = view->g_buffer_->G_depth_.Raw();

    volprims_params->RWVolumeRayToTraceCount = volume_ray_to_trace_count.Raw();
    volprims_params->RWVolumeRayToTraceDirectionBuffer = volume_ray_to_trace_direction.Raw();
    volprims_params->RWVolumeRayToTraceStateBuffer = volume_ray_to_trace_state.Raw();
    volprims_params->RWVolumeRayToTraceOriginBuffer = volume_ray_to_trace_origin.Raw();
    volprims_params->RWVolumeRayToTraceTMaxBuffer = volume_ray_to_trace_tmax.Raw();
    volprims_params->RWVolumeRayToTraceSampledLightIndexBuffer = volume_ray_to_trace_sampled_light_index.Raw();

    auto volume_ray_to_trace_pixel_index = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(uint32_t)
    );
    volprims_params->RWVolumeRayToTracePixelIndexBuffer = volume_ray_to_trace_pixel_index.Raw();
    auto volume_ray_to_trace_transmittance = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, num_screen_pixels * sizeof(float)
    );
    volprims_params->VolumeRayToTraceTransmittanceBuffer = volume_ray_to_trace_transmittance.Raw();

    volprims_params->RWVolumeDirectLightingTexture = view->volume_direct_lighting_->radiance.Raw();

    // Volume Direct Lighting
    {
        auto shader = lib.GetShader<VolumeDirectLightingClearCounters>(ini);
        Helpers::AddComputePass<VolumeDirectLightingClearCounters>(builder, shader, volprims_params);
    }
    {
        auto shader = lib.GetShader<VolumeDirectLightingSpawnLightSamplesShader>(ini);
        auto tile_size = VolumeDirectLightingSpawnLightSamplesShader::kTileSize;
        auto num_groups_x = DivideAndRoundUp(view->film_width_, tile_size);
        auto num_groups_y = DivideAndRoundUp(view->film_height_, tile_size);
        Helpers::AddComputePass<VolumeDirectLightingSpawnLightSamplesShader>(
            builder, shader, volprims_params, num_groups_x, num_groups_y
        );
    }

    {
        // HWRT
        Render_HardwareTransmittanceRayTracing(
            view, builder,
            volume_ray_to_trace_count.Raw(),
            nullptr,
            volume_ray_to_trace_direction.Raw(),
            volume_ray_to_trace_state.Raw(),
            nullptr,
            volume_ray_to_trace_origin.Raw(),
            volume_ray_to_trace_tmax.Raw(),
            volume_ray_to_trace_transmittance.Raw()
        );
    }

    auto wave_size = RHI::Get().GetDeviceProperties().wave_size;
    auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, volume_ray_to_trace_count.Raw(), wave_size);
    {
        auto shader = lib.GetShader<RenderVolumeDirectLightingShader>(ini);
        Helpers::AddComputeIndirectPass<RenderVolumeDirectLightingShader>(
            builder, shader, volprims_params, cmd.Raw()
        );
    }
}

MI_NAMESPACE_END