/*
 * Created: 2025/7/31
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include <renderer/mi_renderer.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>

#include "r_view_common.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_static_mesh.h"

MI_NAMESPACE_BEGIN

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

        SHADER_RESOURCE_PARAMETER(Texture2D, G_DepthTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointClampSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "USE_SCREEN_COORDS", // This shader can be compiled with or without origins as screen coordinates
            "USE_RAY_TMAX_BUFFER" // Sometimes the shader allows extra input to specify the TMax values for rays
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
    auto & lib = RDGShaderLibrary::Get();
    auto ini = RDGShaderInitializationInfo {};
    if (ray_to_trace_origin_screen_coords) ini.optional_macros.push_back("USE_SCREEN_COORDS");
    if (ray_to_trace_tmax) ini.optional_macros.push_back("USE_RAY_TMAX_BUFFER");
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

    params->G_DepthTexture = view->G_depth_.Raw();
    params->PointClampSampler = RHI::Get().GetGlobalSamplers().point_clamp;
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

    params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
    params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());

    auto cmd = Helpers::SpawnTraceRaysIndirectCommand1D(builder, shader, ray_to_trace_list_length);

    builder.AddPass<TraceShadowRaysShader>(
    {}, shader, params,
    [shader, params, view, in_cmd = cmd.Raw()](RDGPass * pass, RHICommandQueueGraphics & queue) {
        RDGCommandHelper::DispatchRaysIndirect<TraceShadowRaysShader>(
            queue, pass, shader, params, in_cmd
        );
        }
    )->AddBuffer(cmd.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
}

class TraceTransmittanceRaysShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListLengthBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceListBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceDirectionBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWRayToTraceStateBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceOriginBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RayToTraceTMaxBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RWRayToTraceTransmittanceBuffer)

        SHADER_RESOURCE_PARAMETER(AccelerationStructure, TLAS)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)

        SHADER_RESOURCE_PARAMETER(Texture2D, G_DepthTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointClampSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()

    static std::vector<std::string> GetShaderOptionalMacros() {
        return {
            "USE_SCREEN_COORDS", // This shader can be compiled with or without origins as screen coordinates
            "USE_RAY_TMAX_BUFFER" // Sometimes the shader allows extra input to specify the TMax values for rays
        };
    }
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(TraceTransmittanceRaysShader, "mi/renderer/shaders/TraceTransmittanceRays.hlsl",
    "TraceTransmittanceRaysRaygen", "TraceTransmittanceRaysClosestHit", "TraceTransmittanceRaysAnyHit", "TraceTransmittanceRaysMiss");

void Renderer::Render_HardwareTransmittanceRayTracing(
    RendererView *view, RenderGraphBuilder &builder,
    RDGBuffer *ray_to_trace_list_length, RDGBuffer *ray_to_trace_list,
    RDGBuffer *ray_to_trace_direction, RDGBuffer *ray_to_trace_state,
    RDGBuffer *ray_to_trace_origin, RDGBuffer *ray_to_trace_tmax,
    RDGBuffer *ray_to_trace_transmittance) {
    auto & lib = RDGShaderLibrary::Get();
    auto ini = RDGShaderInitializationInfo {};
    if (ray_to_trace_tmax) ini.optional_macros.push_back("USE_RAY_TMAX_BUFFER");
    auto shader = lib.GetShader<TraceTransmittanceRaysShader>(ini);
    auto params = builder.Allocate<TraceTransmittanceRaysShader::Params>();
    params->View = view->view_common_params_;
    params->RayToTraceListLengthBuffer = ray_to_trace_list_length;
    params->RayToTraceListBuffer = ray_to_trace_list;
    params->RayToTraceDirectionBuffer = ray_to_trace_direction;
    params->RWRayToTraceStateBuffer = ray_to_trace_state;
    params->RayToTraceOriginBuffer = ray_to_trace_origin;
    params->RayToTraceTMaxBuffer = ray_to_trace_tmax;
    params->RWRayToTraceTransmittanceBuffer = ray_to_trace_transmittance;

    params->G_DepthTexture = view->G_depth_.Raw();
    params->PointClampSampler = RHI::Get().GetGlobalSamplers().point_clamp;
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

    params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
    params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());

    auto cmd = Helpers::SpawnTraceRaysIndirectCommand1D(builder, shader, ray_to_trace_list_length);

    builder.AddPass<TraceTransmittanceRaysShader>(
    {}, shader, params,
    [shader, params, view, in_cmd = cmd.Raw()](RDGPass * pass, RHICommandQueueGraphics & queue) {
        RDGCommandHelper::DispatchRaysIndirect<TraceTransmittanceRaysShader>(
            queue, pass, shader, params, in_cmd
        );
        }
    )->AddBuffer(cmd.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead, RHIPipelineStageFlagBits::kIndirect);
}


MI_NAMESPACE_END