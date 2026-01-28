/*
 * Created: 2025/7/31
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "r_hwrt.h"

#include <ranges>
#include <renderer/mi_renderer.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>

#include "../include/renderer/r_geometry_buffer.h"
#include "r_view_common.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_texture.h"
#include "renderer/mi_volume_primitives.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_gaussian_radiance_field.h"
MI_NAMESPACE_BEGIN
    static CVar<float> CVar_VolumeScatteringEventShellHitCullingBias(
    "r.hwrt.volume_scattering_event_shell_hit_culling_bias",
    "Bias to apply when culling volume scattering events for acceleration."
    "Smaller values trade performance with quality. Better setting this close to a x radius of volume primitives ( a > 1 ).",
    0.05f
);
class TraceShadowRaysShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListLengthBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceDirectionBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceStateBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginScreenCoordBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceTMaxBuffer)

        SHADER_RESOURCE_PARAMETER(AccelerationStructure, TLAS)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianRadianceFieldHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumeGridHeaderBuffer)

        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "USE_SCREEN_COORDS", // This shader can be compiled with or without origins as screen coordinates
            "USE_RAY_TMAX_BUFFER", // Sometimes the shader allows extra input to specify the TMax values for rays
            "USE_RAY_LIST" // Sometimes the rays are not compact, ray indices are stored in a list to be traced
        };
    }
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(TraceShadowRaysShader, "mi/renderer/shaders/TraceShadowRays.hlsl",
    "TraceShadowRaysRaygen", "TraceShadowRaysClosestHit", "TraceShadowRaysAnyHit", "TraceShadowRaysMiss");

void Renderer::Render_HardwareShadowRayTracing(
    RendererView *view, RenderGraphBuilder &builder,
    RDGBuffer *ray_to_trace_list_length, RDGBuffer *ray_to_trace_list,
    RDGBuffer *ray_to_trace_direction, RDGBuffer *ray_to_trace_state,
    RDGBuffer *ray_to_trace_origin_screen_coords, RDGBuffer *ray_to_trace_origin,
    RDGBuffer *ray_to_trace_tmax
) {
    if (ray_to_trace_origin && ray_to_trace_origin_screen_coords) {
        mi_assert(false, "Only one of ray_to_trace_origin or ray_to_trace_origin_screen_coords should be provided.");
    }
    if (!ray_to_trace_origin && !ray_to_trace_origin_screen_coords) {
        mi_assert(false, "Either ray_to_trace_origin or ray_to_trace_origin_screen_coords must be provided.");
    }
    mi_assert(ray_to_trace_list_length && ray_to_trace_direction && ray_to_trace_state, "Essential tracing buffers must be provided.");
    auto & lib = RDGShaderLibrary::Get();
    auto ini = RDGShaderInitializationInfo {};
    if (ray_to_trace_origin_screen_coords) ini.optional_macros.push_back("USE_SCREEN_COORDS");
    if (ray_to_trace_tmax) ini.optional_macros.push_back("USE_RAY_TMAX_BUFFER");
    if (ray_to_trace_list) ini.optional_macros.push_back("USE_RAY_LIST");
    auto shader = lib.GetShader<TraceShadowRaysShader>(ini);
    auto params = builder.Allocate<TraceShadowRaysShader::Params>();
    params->View = view->view_common_params_;
    params->RayToTraceListLengthBuffer = ray_to_trace_list_length;
    params->RayToTraceListBuffer = ray_to_trace_list;
    params->RayToTraceDirectionBuffer = ray_to_trace_direction;
    params->RWRayToTraceStateBuffer = ray_to_trace_state;
    params->RayToTraceOriginScreenCoordBuffer = ray_to_trace_origin_screen_coords;
    params->RayToTraceOriginBuffer = ray_to_trace_origin;
    params->RayToTraceTMaxBuffer = ray_to_trace_tmax;

    params->G_Depth = view->g_buffer_->G_depth_.Raw();
    params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

    params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
    params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
    params->Gaussian3DBuffer = builder.Import(device_allocator_->GetCustomUberBuffer(GaussianRadianceField::kGaussianRadianceAllocatorUberBufferIndex)->GetRHI());
    params->GaussianRadianceFieldHeaderBuffer = builder.Import(device_allocator_->GetGaussianRadianceFieldHeaderBuffer());
    params->VolumeGridHeaderBuffer = builder.Import(device_allocator_->GetVolumeGridHeaderBuffer());

    auto cmd = Helpers::SpawnTraceRaysIndirectCommand1D(builder, shader, ray_to_trace_list_length);
    Helpers::AddTraceRaysIndirectPass(builder, shader, params, cmd.Raw());
}

class TraceTransmittanceRaysShader : public RDGShader {
public:
    struct TraceTransmittanceRaysUB {
        uint32_t Seed;
        glm::uvec3 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(TraceTransmittanceRaysUB, UB)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListLengthBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceDirectionBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceStateBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginScreenCoordBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceTMaxBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceTransmittanceBuffer)

        SHADER_RESOURCE_PARAMETER(AccelerationStructure, TLAS)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumePrimitivesHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)

        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianRadianceFieldHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumeGridHeaderBuffer)

        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "USE_SCREEN_COORDS", // This shader can be compiled with or without origins as screen coordinates
            "USE_RAY_TMAX_BUFFER", // Sometimes the shader allows extra input to specify the TMax values for rays
            "USE_RAY_LIST" // Sometimes the rays are not compact, ray indices are stored in a list to be traced
        };
    }
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(TraceTransmittanceRaysShader, "mi/renderer/shaders/TraceTransmittanceRays.hlsl",
    "TraceTransmittanceRaysRaygen", "TraceTransmittanceRaysClosestHit", "TraceTransmittanceRaysAnyHit", "TraceTransmittanceRaysMiss");

// More than shadow rays, transmittance rays can evaluate the transmittance from 0 and TMax.
// NOTE: if the transmittance ray hits an opaque object and early terminates, the evaluated
// transmittance is always 0.
void Renderer::Render_HardwareTransmittanceRayTracing(
    RendererView *view, RenderGraphBuilder &builder,
    RDGBuffer *ray_to_trace_list_length, RDGBuffer *ray_to_trace_list,
    RDGBuffer *ray_to_trace_direction, RDGBuffer *ray_to_trace_state,
    RDGBuffer *ray_to_trace_origin_screen_coords, RDGBuffer *ray_to_trace_origin,
    RDGBuffer *ray_to_trace_tmax,
    RDGBuffer *ray_to_trace_transmittance,
    uint32_t seed) {
    mi_assert(ray_to_trace_list_length && ray_to_trace_direction && ray_to_trace_state, "Essential tracing buffers must be provided.");
    auto & lib = RDGShaderLibrary::Get();
    auto ini = RDGShaderInitializationInfo {};
    mi_assert(ray_to_trace_origin || ray_to_trace_origin_screen_coords,
        "Either ray_to_trace_origin or ray_to_trace_origin_screen_coords must be provided.");
    mi_assert(!(ray_to_trace_origin_screen_coords && ray_to_trace_origin),
        "Either ray_to_trace_origin or ray_to_trace_origin_screen_coords must be provided, not both.");
    if (ray_to_trace_origin_screen_coords) ini.optional_macros.push_back("USE_SCREEN_COORDS");
    if (ray_to_trace_tmax) ini.optional_macros.push_back("USE_RAY_TMAX_BUFFER");
    if (ray_to_trace_list) ini.optional_macros.push_back("USE_RAY_LIST");
    auto shader = lib.GetShader<TraceTransmittanceRaysShader>(ini);
    auto params = builder.Allocate<TraceTransmittanceRaysShader::Params>();
    params->View = view->view_common_params_;
    auto UB = builder.Allocate<TraceTransmittanceRaysShader::TraceTransmittanceRaysUB>();
    {
        UB->Seed = seed;
    }
    params->UB = UB;
    params->RayToTraceListLengthBuffer = ray_to_trace_list_length;
    params->RayToTraceListBuffer = ray_to_trace_list;
    params->RayToTraceDirectionBuffer = ray_to_trace_direction;
    params->RWRayToTraceStateBuffer = ray_to_trace_state;
    params->RayToTraceOriginScreenCoordBuffer = ray_to_trace_origin_screen_coords;
    params->RayToTraceOriginBuffer = ray_to_trace_origin;
    params->RayToTraceTMaxBuffer = ray_to_trace_tmax;
    params->RWRayToTraceTransmittanceBuffer = ray_to_trace_transmittance;

    params->G_Depth = view->g_buffer_->G_depth_.Raw();
    params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

    params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
    params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
    params->VolumePrimitivesHeaderBuffer = builder.Import(device_allocator_->GetVolumePrimitivesHeaderBuffer());
    params->PrimitiveData = builder.Import(
        device_allocator_->GetCustomUberBuffer(VolumePrimitives::kVolumePrimitiveAllocatorUberBufferIndex)->GetRHI()
    );
    params->Gaussian3DBuffer = builder.Import(device_allocator_->GetCustomUberBuffer(GaussianRadianceField::kGaussianRadianceAllocatorUberBufferIndex)->GetRHI());
    params->GaussianRadianceFieldHeaderBuffer = builder.Import(device_allocator_->GetGaussianRadianceFieldHeaderBuffer());
    params->VolumeGridHeaderBuffer = builder.Import(device_allocator_->GetVolumeGridHeaderBuffer());

    auto cmd = Helpers::SpawnTraceRaysIndirectCommand1D(builder, shader, ray_to_trace_list_length);

    Helpers::AddTraceRaysIndirectPass(builder, shader, params, cmd.Raw());
}

class TraceRadianceRaysShader : public RDGShader {
public:
    struct TraceRadianceRaysUB {
        uint32_t Seed;
        glm::uvec3 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(TraceRadianceRaysUB, UB)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListLengthBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceDirectionBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceStateBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginScreenCoordBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceTMaxBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceResultBuffer)

        SHADER_RESOURCE_PARAMETER(AccelerationStructure, TLAS)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumePrimitivesHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)

        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianRadianceFieldHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumeGridHeaderBuffer)

        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "USE_SCREEN_COORDS", // This shader can be compiled with or without origins as screen coordinates
            "USE_RAY_TMAX_BUFFER", // Sometimes the shader allows extra input to specify the TMax values for rays
            "USE_RAY_LIST" // Sometimes the rays are not compact, ray indices are stored in a list to be traced
        };
    }
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(TraceRadianceRaysShader, "mi/renderer/shaders/TraceRadianceRays.hlsl",
    "TraceRadianceRaysRaygen", "TraceRadianceRaysClosestHit", "TraceRadianceRaysAnyHit", "TraceRadianceRaysMiss");


void Renderer::Render_HardwareRadianceRayTracing(
    RendererView *view, RenderGraphBuilder &builder,
    RDGBuffer *ray_to_trace_list_length, RDGBuffer *ray_to_trace_list,
    RDGBuffer *ray_to_trace_direction, RDGBuffer *ray_to_trace_state,
    RDGBuffer *ray_to_trace_origin_screen_coords, RDGBuffer *ray_to_trace_origin,
    RDGBuffer *ray_to_trace_tmax, RDGBuffer *ray_to_trace_result,
    uint32_t seed
) {

    mi_assert(ray_to_trace_list_length && ray_to_trace_direction && ray_to_trace_state, "Essential tracing buffers must be provided.");
    auto & lib = RDGShaderLibrary::Get();
    auto ini = RDGShaderInitializationInfo {};
    mi_assert(ray_to_trace_origin || ray_to_trace_origin_screen_coords,
        "Either ray_to_trace_origin or ray_to_trace_origin_screen_coords must be provided.");
    mi_assert(!(ray_to_trace_origin_screen_coords && ray_to_trace_origin),
        "Either ray_to_trace_origin or ray_to_trace_origin_screen_coords must be provided, not both.");
    if (ray_to_trace_origin_screen_coords) ini.optional_macros.push_back("USE_SCREEN_COORDS");
    if (ray_to_trace_tmax) ini.optional_macros.push_back("USE_RAY_TMAX_BUFFER");
    if (ray_to_trace_list) ini.optional_macros.push_back("USE_RAY_LIST");
    auto shader = lib.GetShader<TraceRadianceRaysShader>(ini);
    auto params = builder.Allocate<TraceRadianceRaysShader::Params>();
    params->View = view->view_common_params_;
    auto UB = builder.Allocate<TraceRadianceRaysShader::TraceRadianceRaysUB>();
    {
        UB->Seed = seed;
    }
    params->UB = UB;
    params->RayToTraceListLengthBuffer = ray_to_trace_list_length;
    params->RayToTraceListBuffer = ray_to_trace_list;
    params->RayToTraceDirectionBuffer = ray_to_trace_direction;
    params->RWRayToTraceStateBuffer = ray_to_trace_state;
    params->RayToTraceOriginScreenCoordBuffer = ray_to_trace_origin_screen_coords;
    params->RayToTraceOriginBuffer = ray_to_trace_origin;
    params->RayToTraceTMaxBuffer = ray_to_trace_tmax;
    params->RWRayToTraceResultBuffer = ray_to_trace_result;

    params->G_Depth = view->g_buffer_->G_depth_.Raw();
    if (view->scene_->GetSkyTexture()) {
        params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
    } else {
        params->EnvironmentMap = nullptr;
    }
    params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

    params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
    params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
    params->VolumePrimitivesHeaderBuffer = builder.Import(device_allocator_->GetVolumePrimitivesHeaderBuffer());
    params->PrimitiveData = builder.Import(
        device_allocator_->GetCustomUberBuffer(VolumePrimitives::kVolumePrimitiveAllocatorUberBufferIndex)->GetRHI()
    );
    params->Gaussian3DBuffer = builder.Import(device_allocator_->GetCustomUberBuffer(GaussianRadianceField::kGaussianRadianceAllocatorUberBufferIndex)->GetRHI());
    params->GaussianRadianceFieldHeaderBuffer = builder.Import(device_allocator_->GetGaussianRadianceFieldHeaderBuffer());
    params->VolumeGridHeaderBuffer = builder.Import(device_allocator_->GetVolumeGridHeaderBuffer());

    auto cmd = Helpers::SpawnTraceRaysIndirectCommand1D(builder, shader, ray_to_trace_list_length);

    Helpers::AddTraceRaysIndirectPass(builder, shader, params, cmd.Raw());
}

class TraceVisibilityRaysShader : public RDGShader {
public:
    struct TraceVisibilityRaysUB {
        uint32_t Seed;
        float VolumeScatteringEventShellHitCullingBias;
        glm::uvec2 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(TraceVisibilityRaysUB, UB)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListLengthBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceDirectionBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceStateBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginScreenCoordBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceTMaxBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceResultBuffer)

        SHADER_RESOURCE_PARAMETER(AccelerationStructure, TLAS)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumePrimitivesHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)

        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Gaussian3DBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianRadianceFieldHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GaussianSHBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumeGridHeaderBuffer)

        SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
        SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderDefaultMacros() {
        return {};
    }

    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "USE_SCREEN_COORDS", // This shader can be compiled with or without origins as screen coordinates
            "USE_RAY_TMAX_BUFFER", // Sometimes the shader allows extra input to specify the TMax values for rays
            "USE_RAY_LIST", // Sometimes the rays are not compact, ray indices are stored in a list to be traced
            "VISIBILITY_TRACE_TYPE=0", // VISIBILITY_TRACE_TYPE_COARSE
            "VISIBILITY_TRACE_TYPE=1", // VISIBILITY_TRACE_TYPE_COARSE_WITH_EXACT_VOLUME_SCATTERING
        };
    }
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(TraceVisibilityRaysShader, "mi/renderer/shaders/TraceVisibilityRays.hlsl",
    "TraceVisibilityRaysRaygen", "TraceVisibilityRaysClosestHit", "TraceVisibilityRaysAnyHit", "TraceVisibilityRaysMiss");


void Renderer::Render_HardwareVisibilityRayTracing(
    RendererView *view, RenderGraphBuilder &builder,
    RDGBuffer *ray_to_trace_list_length, RDGBuffer *ray_to_trace_list,
    RDGBuffer *ray_to_trace_direction, RDGBuffer *ray_to_trace_state,
    RDGBuffer *ray_to_trace_origin_screen_coords, RDGBuffer *ray_to_trace_origin,
    RDGBuffer *ray_to_trace_tmax, RDGBuffer *ray_to_trace_result,
    uint32_t seed, VisibilityTraceType trace_type
) {
    if (trace_type == VisibilityTraceType::kFull) {
        mi_assert(false, "Not implemented");
    }
    mi_assert(ray_to_trace_list_length && ray_to_trace_direction && ray_to_trace_state, "Essential tracing buffers must be provided.");
    auto & lib = RDGShaderLibrary::Get();
    auto ini = RDGShaderInitializationInfo {};
    mi_assert(ray_to_trace_origin || ray_to_trace_origin_screen_coords,
        "Either ray_to_trace_origin or ray_to_trace_origin_screen_coords must be provided.");
    mi_assert(!(ray_to_trace_origin_screen_coords && ray_to_trace_origin),
        "Either ray_to_trace_origin or ray_to_trace_origin_screen_coords must be provided, not both.");
    if (ray_to_trace_origin_screen_coords) ini.optional_macros.push_back("USE_SCREEN_COORDS");
    if (ray_to_trace_tmax) ini.optional_macros.push_back("USE_RAY_TMAX_BUFFER");
    if (ray_to_trace_list) ini.optional_macros.push_back("USE_RAY_LIST");
    auto trace_type_value = static_cast<uint32_t>(trace_type);
    ini.optional_macros.push_back("VISIBILITY_TRACE_TYPE=" + std::to_string(trace_type_value));
    auto shader = lib.GetShader<TraceVisibilityRaysShader>(ini);
    auto params = builder.Allocate<TraceVisibilityRaysShader::Params>();
    params->View = view->view_common_params_;
    auto UB = builder.Allocate<TraceVisibilityRaysShader::TraceVisibilityRaysUB>();
    {
        UB->Seed = seed;
        UB->VolumeScatteringEventShellHitCullingBias = CVar_VolumeScatteringEventShellHitCullingBias.Get();
    }
    params->UB = UB;
    params->RayToTraceListLengthBuffer = ray_to_trace_list_length;
    params->RayToTraceListBuffer = ray_to_trace_list;
    params->RayToTraceDirectionBuffer = ray_to_trace_direction;
    params->RWRayToTraceStateBuffer = ray_to_trace_state;
    params->RayToTraceOriginScreenCoordBuffer = ray_to_trace_origin_screen_coords;
    params->RayToTraceOriginBuffer = ray_to_trace_origin;
    params->RayToTraceTMaxBuffer = ray_to_trace_tmax;
    params->RWRayToTraceResultBuffer = ray_to_trace_result;

    params->G_Depth = view->g_buffer_->G_depth_.Raw();
    if (view->scene_->GetSkyTexture()) {
        params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
    } else {
        params->EnvironmentMap = nullptr;
    }
    params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

    params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
    params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
    params->VolumePrimitivesHeaderBuffer = builder.Import(device_allocator_->GetVolumePrimitivesHeaderBuffer());
    params->PrimitiveData = builder.Import(
        device_allocator_->GetCustomUberBuffer(VolumePrimitives::kVolumePrimitiveAllocatorUberBufferIndex)->GetRHI()
    );
    params->Gaussian3DBuffer = builder.Import(device_allocator_->GetCustomUberBuffer(GaussianRadianceField::kGaussianRadianceAllocatorUberBufferIndex)->GetRHI());
    params->GaussianRadianceFieldHeaderBuffer = builder.Import(device_allocator_->GetGaussianRadianceFieldHeaderBuffer());
    params->GaussianSHBuffer = builder.Import(device_allocator_->GetCustomUberBuffer(GaussianRadianceField::kGaussianRadianceSHAllocatorUberBufferIndex)->GetRHI());
    params->VolumeGridHeaderBuffer = builder.Import(device_allocator_->GetVolumeGridHeaderBuffer());

    auto cmd = Helpers::SpawnTraceRaysIndirectCommand1D(builder, shader, ray_to_trace_list_length);

    Helpers::AddTraceRaysIndirectPass(builder, shader, params, cmd.Raw());
}

MI_NAMESPACE_END