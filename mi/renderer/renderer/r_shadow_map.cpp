/*
 * Created: 2025/9/15
 * Author:  didu
 * See LICENSE for licensing.
 */

#include <ranges>
#include <rhi/rhi_buffer.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>

#include <renderer/mi_renderer.h>
#include <renderer/mi_material.h>
#include <renderer/mi_renderable.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_static_mesh.h>
#include <renderer/mi_buffer_heap.h>

#include "r_view_common.h"

MI_NAMESPACE_BEGIN

class DrawShadowMapShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(DirectionalLightForShadowMap, LightView)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableNormalTransformBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableIndexAndDescriptorIndexBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshDescriptionBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, StaticMeshHeaderBuffer)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, MaterialHeaderBuffer)

        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)

        SHADER_VERTEX_BUFFER(sizeof(DefaultStaticMeshVertex), VertexBuffer)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Position), RHIVertexAttributeFormatType::k3xFp32, position)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Normal), RHIVertexAttributeFormatType::k3xFp32, normal)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, UV), RHIVertexAttributeFormatType::k2xFp32, uv)

        SHADER_RENDER_TARGET(PixelFormatType::kR32G32_FLOAT, Moments)
        SHADER_RENDER_TARGET(PixelFormatType::kD32_FLOAT, Depth, {})
        END_SHADER_PARAMETERS()
        RDG_SHADER_USE_PARAMETERS(Params)
        DECLARE_SHADER()

    static RDGShaderPipelineConfig GetShaderPipelineConfig() {
        RDGShaderPipelineConfig config{};
        // Reversed-z depth buffer
        config.depth_compare_op = RHIDepthCompareOpType::kLess;
        return config;
    }
};

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawShadowMapShader, "mi/renderer/shaders/ShadowMap.hlsl", "Shadow_VS_Main", "Shadow_PS_Main");

static std::array<glm::vec3, 8> GetCorners(const glm::vec3& min, const glm::vec3& max) {
    return {
        glm::vec3(min.x, min.y, min.z),
        glm::vec3(max.x, min.y, min.z),
        glm::vec3(min.x, max.y, min.z),
        glm::vec3(max.x, max.y, min.z),
        glm::vec3(min.x, min.y, max.z),
        glm::vec3(max.x, min.y, max.z),
        glm::vec3(min.x, max.y, max.z),
        glm::vec3(max.x, max.y, max.z)
    };
}

static glm::mat4 ComputeDirectionalLightWorldToNDC(
    const glm::vec3& lightDirWS,
    const glm::vec3& AABBmin,
    const glm::vec3& AABBmax)
{
    glm::vec3 forward = glm::normalize(lightDirWS);
	glm::vec3 up = { 0.0f, 1.0f, 0.0f };
    glm::vec3 center = (AABBmin + AABBmax) * 0.5f;
    glm::vec3 eye = center - forward;
    glm::mat4 lightView = glm::lookAt(eye, center, up);

	auto CornersWS = GetCorners(AABBmin, AABBmax);

    glm::vec3 minExtents(FLT_MAX);
    glm::vec3 maxExtents(-FLT_MAX);
    for (auto& c : CornersWS) {
        glm::vec4 ptLS = lightView * glm::vec4(c, 1.0);
        minExtents = glm::min(minExtents, glm::vec3(ptLS));
        maxExtents = glm::max(maxExtents, glm::vec3(ptLS));
    }

	// construct an orthographic projection matrix for the light
    float _left = minExtents.x;
    float _right = maxExtents.x;
    float _bottom = minExtents.y;
    float _top = maxExtents.y;
    float _nearZ = glm::max(glm::min(glm::abs(minExtents.z), glm::abs(maxExtents.z))-0.001f, 0.1f);
    float _farZ =  glm::min(glm::max(glm::abs(minExtents.z), glm::abs(maxExtents.z))+0.001f ,1000.0f);

	glm::mat4 lightProj = glm::orthoRH_ZO(_left, _right, _bottom, _top, _nearZ, _farZ);
    return lightProj * lightView;
}


void Renderer::Render_DrawShadowMap(RendererView* view, RenderGraphBuilder& builder) {
    if (view->shadow_mapping_.use_world_bounds_) {
        view->shadow_mapping_.mapping_world_bounds_ = view->scene_->GetAABB();
    }
    auto directional_light_for_shadowMap = builder.Allocate<DirectionalLightForShadowMap>();
    auto world_to_ndc = ComputeDirectionalLightWorldToNDC(
        view->scene_->directional_light_.direction,
	    view->shadow_mapping_.mapping_world_bounds_.min,
	    view->shadow_mapping_.mapping_world_bounds_.max
    );
    view->shadow_mapping_.light_world_to_ndc_ = world_to_ndc;
    directional_light_for_shadowMap->LightWorldToNDC = world_to_ndc;
    directional_light_for_shadowMap->LightDirWS = view->scene_->directional_light_.direction;

    auto params = builder.Allocate<DrawShadowMapShader::Params>();
    params->LightView = directional_light_for_shadowMap;
    params->View = view->view_common_params_;
    params->RenderableHeaderBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
    params->RenderableTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    params->RenderableNormalTransformBuffer = builder.Import(view->scene_->GetDeviceScene()->d_renderable_normal_transforms_.Raw());
    params->RenderableIndexAndDescriptorIndexBuffer = ctx.deferred_static_meshes.d_static_mesh_draw_command_renderable_descriptor_indices.Raw();
    params->MaterialHeaderBuffer = builder.Import(device_allocator_->material_header_buffer_.Raw());

    // params->GeometryHeaderBuffer = builder.Import(device_allocator_->geometry_header_buffer_.Raw());
    params->StaticMeshDescriptionBuffer = builder.Import(device_allocator_->static_mesh_description_uber_buffer_->GetRHI());
    params->StaticMeshHeaderBuffer = builder.Import(device_allocator_->static_mesh_header_buffer_.Raw());

    auto shadow_depth_buffer = RDGTexture::Create2D(
        kDefaultShadowMapResolution, kDefaultShadowMapResolution, PixelFormatType::kD32_FLOAT,
        RHITextureUsageFlagBits::kDepthStencil);
    params->Depth = shadow_depth_buffer.Raw();
    params->Depth.load_op = RHILoadOpType::kClear;
    params->Depth.clear_value = { 1.0f, 1.0f, 1.0f, 1.0f };
    params->Moments = view->shadow_map_moments_.Raw();
    params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;

    auto shader = RDGShaderLibrary::Get().GetShader<DrawShadowMapShader>();

    // Rasterize static meshes with batched drawing
    auto raster_pass = builder.AddPass<DrawShadowMapShader>({}, shader, params,
        [params, shader, data = ctx.deferred_static_meshes, rdg_draw_cmd = ctx.deferred_static_meshes.d_static_draw_commands.Raw()]
        ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            auto tid = RDGCommandHelper::CreateParameterTable(queue, pass, shader, params);
            RDGCommandHelper::BeginGraphicsRender(queue, shader, tid,
                &DrawShadowMapShader::GetShaderParamStructInfo()->render_pass_info_, params);
            queue.SetCullMode(RHICullModeType::kBack);
            RHIBuffer * last_vertex_buffer {};
            RHIBuffer * last_index_buffer {};
            RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
            for (int i = 0; i < (int)data.draw_indirect_commands.size(); i++) {
                auto & hdr = data.draw_invocation_sorting_headers[i];
                if (last_vertex_buffer != hdr.vertex_buffer || last_index_buffer != hdr.index_buffer) {
                    if (i > 0) {
                        auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                        queue.DrawIndexedIndirect(
                            data.draw_invocation_sorting_headers[i-1].index_buffer->GetSpan(),
                            cmd_span,  i - first_cmd
                        );
                        cmd_span.offset = i * sizeof(RHIDrawIndexedIndirectCommand);
                    }
                    last_vertex_buffer = hdr.vertex_buffer;
                    last_index_buffer = hdr.index_buffer;
                    queue.BindVertexBuffer(0, hdr.vertex_buffer->GetSpan());
                }
            }
            if (!data.draw_indirect_commands.empty()) {
                int i = (int)data.draw_indirect_commands.size();
                auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                queue.DrawIndexedIndirect(
                    data.draw_invocation_sorting_headers[i-1].index_buffer->GetSpan(),
                    cmd_span,  i - first_cmd);
            }
            RDGCommandHelper::EndGraphicsRender(queue);
        }
    );

    // Place barriers for geometry / indirect buffers manually
    {
        // TODO utilize DeviceBindlessAllocator, place less barriers.
        std::set<RHIBuffer*> barrier_buffers;
        for (auto& e : ctx.visible_renderables) {
            if (!e->IsDirty()) continue;
            if (auto mesh_instance = e->As<StaticMeshInstance>()) {
                auto mesh = mesh_instance->GetStaticMesh();
                for (auto geom : mesh->GetGeometries()) {
                    if (auto dev = geom->GetDeviceGeometry()) {
                        if (auto vb = dev->GetDeviceVertexBuffer()) barrier_buffers.insert(vb->GetRHI().buffer);
                        if (auto ib = dev->GetDeviceIndexBuffer()) barrier_buffers.insert(ib->GetRHI().buffer);
                    }
                }
            }
        }
        for (auto e : barrier_buffers) {
            // Destructors of temporaries created in one line of code will destruct after the line
            raster_pass->AddBufferH(
                builder.Import(e, RHIGPUAccessFlagBits::kTransferWrite, RHIPipelineStageFlagBits::kTransfer),
                RHIGPUAccessFlagBits::kVertexAttributeRead | RHIGPUAccessFlagBits::kIndexRead
            );
        }

        // Indirect command
        raster_pass->AddBufferH(ctx.deferred_static_meshes.d_static_draw_commands.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead);
    }
}

MI_NAMESPACE_END