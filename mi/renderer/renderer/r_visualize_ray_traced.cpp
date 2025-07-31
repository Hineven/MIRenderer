/*
 * Created: 2025/7/17
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/common.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_resource.h"
#include "rdg/rdg_shader.h"
#include "renderer/mi_renderer.h"
#include "r_view_common.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"

MI_NAMESPACE_BEGIN
class RayTracingVisualizationShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(AccelerationStructure, TLAS)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDebugOutput)
        SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(RayTracingVisualizationShader, "mi/renderer/shaders/RayTracingVisualization.hlsl",
                                "RayTracingVisualizationRaygen",
                                 "RayTracingVisualizationClosestHit", "RayTracingVisualizationAnyHit", "RayTracingVisualizationMiss")

void Renderer::Render_VisualizeRayTraced(RendererView *view, RenderGraphBuilder &builder) {
    // Simply overwrite the debug output texture with the ray-traced result.
    view->debug_output_ = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR8G8B8A8_UNORM);
    auto shader = RDGShaderLibrary::Get().GetShader<RayTracingVisualizationShader>();
    auto params = builder.Allocate<RayTracingVisualizationShader::Params>();
    params->View = view->view_common_params_;
    params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->GetStaticMeshDescriptionUberBuffer()->GetRHI());
    params->GeometryHeaderBuffer = builder.Import(device_allocator_->GetGeometryHeaderBuffer());
    params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->GetStaticMeshHeaderBuffer());
    params->VertexBuffer = builder.Import(device_allocator_->GetVertexUberBuffer()->GetRHI());
    params->IndexBuffer = builder.Import(device_allocator_->GetIndexUberBuffer()->GetRHI());
    params->MaterialHeaderBuffer = builder.Import(device_allocator_->GetMaterialHeaderBuffer());
    params->RWDebugOutput = view->debug_output_.Raw();
    params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
    params->LinearSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    builder.AddPass<RayTracingVisualizationShader>(
        {}, params,
        [shader, params, view](RDGPass * pass, RHICommandQueueGraphics & queue) {
            RDGCommandHelper::DispatchRays<RayTracingVisualizationShader>(
                queue, pass, shader, params, view->film_width_, view->film_height_
            );
        }
    );
}

MI_NAMESPACE_END