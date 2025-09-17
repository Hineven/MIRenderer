/*
 * Created: 2025/9/15
 * Author:  didu
 * See LICENSE for licensing.
 */
#include <ranges>
#include <renderer/mi_renderer.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>

#include "r_view_common.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_static_mesh.h"
#include "rhi/rhi_buffer.h"

MI_NAMESPACE_BEGIN

class DrawShadowMapShader : public RDGShader {
public:
	BEGIN_SHADER_PARAMETERS(Params)
		SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
		SHADER_UNIFORM_BUFFER(DirectionalLightForShadowMap, LightView)
		SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableHeaders)
		SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransforms)
		SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableNormalTransforms)
		SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableIndexAndMaterialIndex)

        SHADER_VERTEX_BUFFER(sizeof(DefaultStaticMeshVertex), VertexBuffer)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Position), RHIVertexAttributeFormatType::k3xFp32, position)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, Normal), RHIVertexAttributeFormatType::k3xFp32, normal)
        SHADER_VERTEX_ATTRIBUTE(0, offsetof(DefaultStaticMeshVertex, UV), RHIVertexAttributeFormatType::k2xFp32, uv)
		
		SHADER_RENDER_TARGET(PixelFormatType::kR16G16B16A16_FLOAT, Moments)
	END_SHADER_PARAMETERS()
	RDG_SHADER_USE_PARAMETERS(Params)
	DECLARE_SHADER()
};

IMPLEMENT_RDG_GRAPHICS_SHADER(DrawShadowMapShader, "mi/renderer/shaders/ShadowMap.hlsl", "VS_Main", "PS_Main");



//-----------------------------------------------
// 计算主相机视锥体的 8 个角点（世界空间）
//-----------------------------------------------
std::array<glm::vec3, 8> GetCameraFrustumCornersWS(
    const glm::mat4& proj,
    const glm::mat4& view)
{
    glm::mat4 invViewProj = glm::inverse(proj * view);

    std::array<glm::vec3, 8> corners;
    int idx = 0;
    for (int x = 0; x < 2; x++) {
        for (int y = 0; y < 2; y++) {
            for (int z = 0; z < 2; z++) {
                glm::vec4 ptNDC(
                    2.0f * x - 1.0f,
                    2.0f * y - 1.0f,
                    2.0f * z - 1.0f,
                    1.0f
                );
                glm::vec4 ptWS = invViewProj * ptNDC;
                corners[idx++] = glm::vec3(ptWS) / ptWS.w;
            }
        }
    }
    return corners;
}

//-----------------------------------------------
// 根据 LightDir + 相机视锥体 得到 LightWorldToNDC
//-----------------------------------------------
glm::mat4 ComputeDirectionalLightMatrix(
    const glm::vec3& lightDirWS,   // 世界空间光方向 (必须单位化)
    const glm::mat4& cameraProj,
    const glm::mat4& cameraView)
{
    // Step 1: 得到相机视锥体的 8 个角点（世界空间）
    auto frustumCornersWS = GetCameraFrustumCornersWS(cameraProj, cameraView);

    // Step 2: 构造 Light View 矩阵
    glm::vec3 forward = glm::normalize(-lightDirWS);

    // 选一个合适的 up 向量（避免和 forward 平行）
    glm::vec3 up = glm::abs(forward.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    glm::vec3 right = glm::normalize(glm::cross(up, forward));
    up = glm::cross(forward, right);

    // 让光的相机位置放在视锥体中心偏后
    glm::vec3 center(0);
    for (auto& c : frustumCornersWS) center += c;
    center /= (float)frustumCornersWS.size();

    float dist = 100.0f; // 光相机离场景中心的距离，可调（要保证能看到整个视锥体）
    glm::vec3 eye = center - forward * dist;

    glm::mat4 lightView = glm::lookAt(eye, center, up);

    // Step 3: 把视锥体角点变到光空间，找到包围盒
    glm::vec3 minExtents(FLT_MAX);
    glm::vec3 maxExtents(-FLT_MAX);
    for (auto& c : frustumCornersWS) {
        glm::vec4 ptLS = lightView * glm::vec4(c, 1.0);
        minExtents = glm::min(minExtents, glm::vec3(ptLS));
        maxExtents = glm::max(maxExtents, glm::vec3(ptLS));
    }

    // Step 4: 构造正交投影
    float _left = minExtents.x;
    float _right = maxExtents.x;
    float _bottom = minExtents.y;
    float _top = maxExtents.y;
    float _nearZ = minExtents.z;
    float _farZ = maxExtents.z;

    glm::mat4 lightProj = glm::orthoRH_ZO(_left, _right, _bottom, _top, _nearZ, _farZ);
    
    // Step 5: 最终矩阵
    return lightProj * lightView;
}


void Renderer::Render_DrawShadowMap(RendererView* view, RenderGraphBuilder& builder) {
	//没找见方向光信息，先占个位
    glm::vec3 light_dir = glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f));

	auto directional_light_for_shadowMap = builder.Allocate<DirectionalLightForShadowMap>();
    directional_light_for_shadowMap->LightWorldToNDC = ComputeDirectionalLightMatrix(
        light_dir,
        view->camera_.proj,
        view->camera_.view
	);
	directional_light_for_shadowMap->LightDirWS = light_dir;
	directional_light_for_shadowMap->DepthBias = 0.0005f;
	directional_light_for_shadowMap->SlopeBias = 0.01f;
    
    auto params = builder.Allocate<DrawShadowMapShader::Params>();
	params->RenderableHeaders = builder.Import(view->scene_->GetDeviceScene()->d_renderable_headers_.Raw());
	params->RenderableTransforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
	params->RenderableNormalTransforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_normal_transforms_.Raw());
	params->RenderableIndexAndMaterialIndex = ctx.static_meshes.d_static_mesh_draw_command_renderable_material_indices.Raw();
    
    view->shadow_map_moments_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_,
        PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
	);
    view->shadow_map_moments_->SetName("Shadow Map Moments");
	params->Moments = view->shadow_map_moments_.Raw();

	auto shader = RDGShaderLibrary::Get().GetShader<DrawShadowMapShader>();

    // Rasterize static meshes with batched drawing
    auto raster_pass = builder.AddPass<DrawShadowMapShader>({}, shader, params,
        [params, shader, data = ctx.static_meshes, rdg_draw_cmd = ctx.static_meshes.d_static_draw_commands.Raw()]
        ([[maybe_unused]] RDGPass* pass, RHICommandQueueGraphics& queue) {
            if (RDGCommandHelper::BindGraphicsShader<DrawShadowMapShader>(
                queue, pass, shader, params, true
            )) {
                queue.BeginRendering();
                queue.SetCullMode(RHICullModeType::kBack);
                RHIBuffer* last_vertex_buffer{};
                RHIBuffer* last_index_buffer{};
                RHIBufferSpan cmd_span = rdg_draw_cmd->GetRHI();
                for (int i = 0; i < (int)data.draw_indirect_commands.size(); i++) {
                    auto& hdr = data.draw_invocation_sorting_headers[i];
                    if (last_vertex_buffer != hdr.vertex_buffer || last_index_buffer != hdr.index_buffer) {
                        if (i > 0) {
                            // Batch submit previous commands sharing the same vertex & index buffer settings.
                            auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                            queue.DrawIndexedIndirect(
                                data.draw_invocation_sorting_headers[i - 1].index_buffer->GetSpan(),
                                cmd_span, i - first_cmd
                            );
                            cmd_span.offset = i * sizeof(RHIDrawIndexedIndirectCommand);
                        }
                        last_vertex_buffer = hdr.vertex_buffer;
                        last_index_buffer = hdr.index_buffer;
                        queue.BindVertexBuffer(0, hdr.vertex_buffer->GetSpan());
                    }
                }
                // Submit last batch if not empty
                if (!data.draw_indirect_commands.empty()) {
                    int i = (int)data.draw_indirect_commands.size();
                    // Batch submit previous commands sharing the same vertex & index buffer settings.
                    auto first_cmd = (uint32_t)(cmd_span.offset / sizeof(RHIDrawIndexedIndirectCommand));
                    queue.DrawIndexedIndirect(
                        data.draw_invocation_sorting_headers[i - 1].index_buffer->GetSpan(),
                        cmd_span, i - first_cmd);
                }
                queue.EndRendering();
            }
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
        raster_pass->AddBufferH(ctx.static_meshes.d_static_draw_commands.Raw(), RHIGPUAccessFlagBits::kIndirectCommandRead);
    }
}

MI_NAMESPACE_END