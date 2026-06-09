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
#include "renderer/mi_volume_texture.h"
#include "renderer/mi_volume_grid.h"
#include "r_view_common.h"
#include "r_persistent.h"
#include "r_diffuse_direct_lighting.h"
#include "r_light_structure.h"
#include "r_directional_light.h"
#include "r_light_cluster_hiearchy.h"
#include "../shaders/shared/SharedLight.hlsl"
#include "dlss/ngx_context.h"
#include "dlss/dlss_rr_context.h"

MI_NAMESPACE_BEGIN
static CVar CVar_PathTracingEnableAccumulation(
    "r.pathtracing.enable_accumulation",
    "Enable accumulation for path tracing.",
    false
);
static CVar<int> CVar_MaxNumBounces(
    "r.pathtracing.max_num_bounces",
    "Maximum number of bounces for path tracing.",
    8
);
static CVar<bool> CVar_EnableDLSSRR(
    "r.pathtracing.dlss_rr",
    "Enable DLSS Ray Reconstruction for path tracing (requires RTX GPU + NGX).",
    true
);
static CVar<int> CVar_NEE_Mode(
    "r.pathtracing.nee_mode",
    "NEE mode: 0=off, 1=uniform, 2=LCH (not yet implemented).",
    0
);

struct ReferencePathTracerUB {
    uint FrameIndex;
    uint EnableAccumulation;
    uint MaxNumBounces;
    float EnvironmentMapLOD;
    glm::vec3 EnvironmentMapMultiplier;
    uint NEEEnabled;                    // 0=off, 1=uniform
    uint NumMeshLightInstanceTriangles; // total triangles in MLI triangle buffer
    uint NumAreaLights;                 // total AreaLight entries in LightBuffer
    uint Padding;
};

// --- Standard path tracer (accumulation mode, no DLSS auxiliary outputs) ---

class ReferencePathTracerShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(ReferencePathTracerUB, UB)
        SHADER_UNIFORM_BUFFER(DirectionalLightUniform, DirectionalLight_UB)
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
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, VolumeGridHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, LightBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightInstanceTriangleBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightInstanceBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, LCH_MeshLightBuffer)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWRadiance)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWDepth)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWNormal)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWMotionVector)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWAlbedo)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWSpecularAlbedo)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWRoughness)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWAlpha)
        SHADER_RESOURCE_PARAMETER(TextureCube, EnvironmentMap)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RESOURCE_PARAMETER(SamplerState, PointWrapSampler)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Params)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_RAY_TRACING_SHADER(ReferencePathTracerShader,
    "mi/renderer/shaders/ReferencePathTracer.hlsl",
    "ReferencePathTracer", "ReferencePathTracer",
    "ReferencePathTracerRaygen", "ReferencePathTracerMiss")

template<typename ShaderParams>
void Renderer::FillPathTracerCommonParams(
    ShaderParams * params,
    RendererView * view,
    RenderGraphBuilder & builder)
{
    params->View = view->view_common_params_;
    auto directional_light_ub = builder.Allocate<DirectionalLightUniform>();
    FillUniformBufferForDirectionalLight(view, directional_light_ub);
    params->DirectionalLight_UB = directional_light_ub;
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
    params->VolumeGridHeaderBuffer = builder.Import(device_allocator_->GetVolumeGridHeaderBuffer());
    params->LightBuffer = builder.Import(device_allocator_->GetAreaLightsUberBuffer()->GetRHI());
    params->LCH_MeshLightInstanceTriangleBuffer = builder.Import(device_allocator_->GetMeshLightInstanceTriangleUberBuffer()->GetRHI());
    params->LCH_MeshLightInstanceBuffer = builder.Import(device_allocator_->GetMeshLightInstanceUberBuffer()->GetRHI());
    params->LCH_MeshLightBuffer = builder.Import(device_allocator_->GetMeshLightUberBuffer()->GetRHI());
    if (view->scene_->GetSkyTexture()) {
        params->EnvironmentMap = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
    } else {
        params->EnvironmentMap = nullptr;
    }
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    params->PointWrapSampler = RHI::Get().GetGlobalSamplers().point_wrap;
}

void Renderer::Render_PathTracing (RendererView *view, RenderGraphBuilder &builder) {
    view->did_render_path_tracing_this_frame_ = true;

    bool use_dlss = CVar_EnableDLSSRR.Get() && ngx_context_ && ngx_context_->IsInitialized();

    if (use_dlss) {
        Render_PathTracingDLSS(view, builder);
    } else {
        Render_PathTracingAccumulation(view, builder);
    }
}

void Renderer::Render_PathTracingAccumulation (RendererView *view, RenderGraphBuilder &builder) {
    auto ini = RDGShaderInitializationInfo {};
    ini.optional_macros.push_back("MAX_NUM_GRID_LIGHTS=" + std::to_string(CVar_MaxNumGridLights.Get()));
    ini.optional_macros.push_back("NUM_LIGHT_SAMPLER_SAMPLES=" + std::to_string(CVar_NumLightSamplerSamples.Get()));
    auto shader = RDGShaderLibrary::Get().GetShader<ReferencePathTracerShader>(ini);

    if (!view->persistent_data_->path_tracing_film_) {
        view->persistent_data_->path_tracing_film_ = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR32G32B32A32_FLOAT,
            RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kTransferSrc);
        view->persistent_data_->path_tracing_film_->SetExport();
    }
    auto params = builder.Allocate<ReferencePathTracerShader::ShaderParameters>();

    auto UB = builder.Allocate<ReferencePathTracerUB>();
    {
        UB->FrameIndex = view->persistent_data_->frame_index_;
        UB->EnableAccumulation = CVar_PathTracingEnableAccumulation.Get() ? 1 : 0;
        UB->MaxNumBounces = glm::clamp(CVar_MaxNumBounces.Get(), 1, 256);
        UB->EnvironmentMapLOD = CVar_EnvironmentLightEvaluateLOD.Get();
        UB->EnvironmentMapMultiplier = CVar_EnvironmentLightMultiplier.Get();
        int nee_mode = CVar_NEE_Mode.Get();
        UB->NEEEnabled = (nee_mode == 1) ? 1u : 0u;
        UB->NumMeshLightInstanceTriangles = (uint)(device_allocator_->GetMeshLightInstanceTriangleUberBuffer()->GetAllocationLimitByteOffset() / sizeof(MeshLightInstanceTriangle));
        UB->NumAreaLights = (uint)(device_allocator_->GetAreaLightsUberBuffer()->GetAllocationLimitByteOffset() / sizeof(AreaLight));
        UB->Padding = 0;
        bool camera_dirty = view->camera_ != view->persistent_data_->prev_camera;
        if (CVar_MaxNumBounces.IsDirty() || camera_dirty) {
            UB->EnableAccumulation = 0;
            CVar_MaxNumBounces.ClearDirty();
        }
    }

    FillPathTracerCommonParams(params, view, builder);
    params->UB = UB;
    params->RWRadiance = view->persistent_data_->path_tracing_film_.Raw();
    params->RWDepth = nullptr;
    params->RWNormal = nullptr;
    params->RWMotionVector = nullptr;
    params->RWAlbedo = nullptr;
    params->RWSpecularAlbedo = nullptr;
    params->RWRoughness = nullptr;
    params->RWAlpha = nullptr;

    Helpers::AddTraceRaysPass(builder, shader, params, view->film_width_, view->film_height_);

    // Blit accumulated result to debug_output_ for downstream DrawToOutput
    // Uses Blit instead of Copy because path_tracing_film_ is R32F while debug_output_ is R16F
    Helpers::BlitTexture(builder, view->persistent_data_->path_tracing_film_.Raw(), view->debug_output_.Raw());
}

void Renderer::Render_PathTracingDLSS (RendererView *view, RenderGraphBuilder &builder) {
    // Ensure per-view DLSS-RR context exists
    if (!view->persistent_data_->dlss_rr_context_) {
        view->persistent_data_->dlss_rr_context_ = TRef<DLSSRRContext>(new DLSSRRContext());
        if (!view->persistent_data_->dlss_rr_context_->Initialize(ngx_context_.Raw(), view->film_width_, view->film_height_)) {
            MI_LOG(MIInfraLogType::kWarning, "DLSS-RR: Failed to initialize for view, falling back to accumulation.");
            view->persistent_data_->dlss_rr_context_ = nullptr;
            Render_PathTracingAccumulation(view, builder);
            return;
        }
    }

    auto ini = RDGShaderInitializationInfo {};
    ini.optional_macros.push_back("MAX_NUM_GRID_LIGHTS=" + std::to_string(CVar_MaxNumGridLights.Get()));
    ini.optional_macros.push_back("NUM_LIGHT_SAMPLER_SAMPLES=" + std::to_string(CVar_NumLightSamplerSamples.Get()));
    ini.optional_macros.push_back("ENABLE_DLSS_RR=1");
    auto shader = RDGShaderLibrary::Get().GetShader<ReferencePathTracerShader>(ini);

    // Noisy input radiance — per-frame, no persistence needed (DLSS handles temporal)
    auto noisy_radiance_ref = builder.CreateTexture2D(view->film_width_, view->film_height_, PixelFormatType::kR16G16B16A16_FLOAT);
    auto * noisy_radiance = noisy_radiance_ref.Raw();

    auto params = builder.Allocate<ReferencePathTracerShader::ShaderParameters>();

    auto UB = builder.Allocate<ReferencePathTracerUB>();
    {
        UB->FrameIndex = view->persistent_data_->frame_index_;
        UB->EnableAccumulation = 0;
        UB->MaxNumBounces = glm::clamp(CVar_MaxNumBounces.Get(), 1, 256);
        UB->EnvironmentMapLOD = CVar_EnvironmentLightEvaluateLOD.Get();
        UB->EnvironmentMapMultiplier = CVar_EnvironmentLightMultiplier.Get();
        int nee_mode = CVar_NEE_Mode.Get();
        UB->NEEEnabled = (nee_mode == 1) ? 1u : 0u;
        UB->NumMeshLightInstanceTriangles = (uint)(device_allocator_->GetMeshLightInstanceTriangleUberBuffer()->GetAllocationLimitByteOffset() / sizeof(MeshLightInstanceTriangle));
        UB->NumAreaLights = (uint)(device_allocator_->GetAreaLightsUberBuffer()->GetAllocationLimitByteOffset() / sizeof(AreaLight));
        UB->Padding = 0;
        CVar_MaxNumBounces.ClearDirty();
    }

    FillPathTracerCommonParams(params, view, builder);
    params->UB = UB;
    params->RWRadiance = noisy_radiance;

    auto * dlss_rr = view->persistent_data_->dlss_rr_context_.Raw();
    params->RWDepth = dlss_rr->GetDepthBuffer();
    params->RWNormal = dlss_rr->GetNormalBuffer();
    params->RWMotionVector = dlss_rr->GetMotionVectorBuffer();
    params->RWAlbedo = dlss_rr->GetAlbedoBuffer();
    params->RWSpecularAlbedo = dlss_rr->GetSpecularAlbedoBuffer();
    params->RWRoughness = dlss_rr->GetRoughnessBuffer();
    params->RWAlpha = dlss_rr->GetAlphaBuffer();

    Helpers::AddTraceRaysPass(builder, shader, params, view->film_width_, view->film_height_);

    // Only reset DLSS-RR history on first frame or resolution change.
    // Camera movement is handled by DLSS internally via motion vectors — do NOT reset on camera dirty.
    bool resolution_changed = dlss_rr->IsResolutionChanged(view->film_width_, view->film_height_);
    bool reset_history = view->persistent_data_->frame_index_ == 0 || resolution_changed;
    dlss_rr->OnResolutionChanged(view->film_width_, view->film_height_);

    auto * dlss_output = dlss_rr->GetDLSSOutput();
    auto * depth = dlss_rr->GetDepthBuffer();
    auto * normal = dlss_rr->GetNormalBuffer();
    auto * motion_vector = dlss_rr->GetMotionVectorBuffer();
    auto * albedo = dlss_rr->GetAlbedoBuffer();
    auto * specular_albedo = dlss_rr->GetSpecularAlbedoBuffer();
    auto * roughness = dlss_rr->GetRoughnessBuffer();
    auto * alpha = dlss_rr->GetAlphaBuffer();
    auto jitter = view->camera_jitter_;

    // Grab view matrices for DLSS-RR (column-major 4x4, matches HLSL float4x4)
    const float * world_to_view = &view->view_common_params_->Camera.WorldToView[0][0];
    const float * view_to_clip   = &view->view_common_params_->Camera.ViewToNDC[0][0];

    builder.AddPass("DLSS Ray Reconstruction", RDGPassFlagBits::kNeverCull,
        [dlss_rr, noisy_radiance, depth, normal, motion_vector, albedo, specular_albedo, roughness, alpha, dlss_output, jitter, reset_history, world_to_view, view_to_clip]
        ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            // Extract Vulkan resource handles on the Render Thread before entering the RHI thread lambda.
            // RDG resource access (GetRHI, RHIGetVulkanTextureInfo) is only valid on the Render Thread.
            auto res_snapshot = dlss_rr->BuildResourceSnapshot(
                noisy_radiance, depth, normal, motion_vector,
                albedo, specular_albedo, roughness, alpha, dlss_output);

            queue.RHIExecute([=]([[maybe_unused]] RHICommandQueueBase & q) {
                dlss_rr->Evaluate_RHI(
                    res_snapshot,
                    jitter.x, jitter.y,
                    reset_history,
                    world_to_view,
                    view_to_clip
                );
            });
        })
        // Use precise storage access flags (not kShaderRead which includes kShaderSampledRead).
        // DLSS-RR reads all inputs as UAV (storage read via Load), not as sampled textures.
        // This avoids false "sampled read + storage RW" warnings and prevents kGeneral layout fallback.
        ->AddTexture(noisy_radiance, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRead, RHIPipelineStageFlagBits::kNone)
        ->AddTexture(depth, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRead, RHIPipelineStageFlagBits::kNone)
        ->AddTexture(normal, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRead, RHIPipelineStageFlagBits::kNone)
        ->AddTexture(motion_vector, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRead, RHIPipelineStageFlagBits::kNone)
        ->AddTexture(albedo, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRead, RHIPipelineStageFlagBits::kNone)
        ->AddTexture(specular_albedo, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRead, RHIPipelineStageFlagBits::kNone)
        ->AddTexture(roughness, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRead, RHIPipelineStageFlagBits::kNone)
        ->AddTexture(alpha, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRead, RHIPipelineStageFlagBits::kNone)
        ->AddTexture(dlss_output, RHITextureLayoutType::kGeneral,
            RHIGPUAccessFlagBits::kShaderStorageRW, RHIPipelineStageFlagBits::kNone);

    // Copy DLSS denoised result to debug_output_ for downstream DrawToOutput
    Helpers::CopyTexture(builder, dlss_output, view->debug_output_.Raw());
}

MI_NAMESPACE_END
