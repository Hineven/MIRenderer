/*
 * Created: 2025/9/13
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
#include "renderer/mi_volume_primitives.h"
#include "r_view_common.h"
#include "r_persistent.h"
#include "r_diffuse_direct_lighting.h"
#include "r_light_structure.h"

MI_NAMESPACE_BEGIN
    static CVar<bool> CVar_PathTracingEnableAccumulation(
    "r.pathtracing.enable_accumulation",
    "Enable accumulation for path tracing.",
    false
);

struct ReferencePathTracerUB {
    uint FrameIndex;
    uint EnableAccumulation;
    uint Padding0;
    uint Padding1;
};

class ReferencePathTracerShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(ReferencePathTracerUB, UB)
        SHADER_RESOURCE_PARAMETER(AccelerationStructure, TLAS)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableInverseTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableNormalTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, GeometryHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VertexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, IndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumePrimitivesHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWRadiance)
        SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(ReferencePathTracerShader,
    "mi/renderer/shaders/ReferencePathTracer.hlsl",
    "ReferencePathTracerRaygen",
    "ReferencePathTracerClosestHit", "ReferencePathTracerAnyHit", "ReferencePathTracerMiss")

void Renderer::Render_PathTracing (RendererView *view, RenderGraphBuilder &builder) {
    // Standalone path tracing renderer
    auto ini = RDGShaderInitializationInfo {};
    ini.optional_macros.push_back("MAX_NUM_GRID_LIGHTS=" + std::to_string(CVar_MaxNumGridLights.Get()));
    ini.optional_macros.push_back("NUM_LIGHT_SAMPELR_SAMPLES=" + std::to_string(CVar_NumLightSamplerSamples.Get()));
    auto shader = RDGShaderLibrary::Get().GetShader<ReferencePathTracerShader>(ini);

    if (!view->persistent_data_->path_tracing_film_) {
        // Use 32bit floats for accumulating large sample counts
        view->persistent_data_->path_tracing_film_ = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR32G32B32A32_FLOAT);
        // Make sure the film is accumulated across frames
        view->persistent_data_->path_tracing_film_->SetExport();
    }
    auto params = builder.Allocate<ReferencePathTracerShader::ShaderParameters>();

    auto UB = builder.Allocate<ReferencePathTracerUB>();
    {
        UB->FrameIndex = view->persistent_data_->frame_index_;
        UB->EnableAccumulation = CVar_PathTracingEnableAccumulation.Get() ? 1 : 0;
    }

    params->View = view->view_common_params_;
    params->UB = UB;
    // 几何与场景资源
    params->TLAS = view->scene_->GetDeviceScene()->TLAS_.Raw();
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    params->RenderableInverseTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_inverse_transforms_.Raw());
    params->RenderableNormalTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_normal_transforms_.Raw());
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
    params->RWRadiance = view->persistent_data_->path_tracing_film_.Raw();
    params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;

    Helpers::AddTraceRaysPass(builder, shader, params, view->film_width_, view->film_height_);
}

MI_NAMESPACE_END