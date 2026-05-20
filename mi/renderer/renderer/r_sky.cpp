/*
 * Created: 2025/5/4
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_scene.h>

#include "../include/renderer/r_geometry_buffer.h"
#include "r_internal_common.h"
#include "r_view_common.h"
#include "renderer/mi_texture.h"

MI_NAMESPACE_BEGIN
class SkyShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(SkyShaderParameters)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(TextureCube, SkyTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, LinearWrapSampler)
        SHADER_RENDER_TARGET(PixelFormatType::kR8G8B8A8_UNORM, Output)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(SkyShaderParameters)
    DECLARE_SHADER()
};
IMPLEMENT_RDG_GRAPHICS_SHADER(SkyShader, "mi/renderer/shaders/Sky.hlsl", "VS_Main", "PS_Main");

void Renderer::Render_DrawSky(RendererView *view, RenderGraphBuilder &builder) {
    auto &lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<SkyShader>();
    auto params = builder.Allocate<SkyShader::SkyShaderParameters>();
    {
        params->Output = view->g_buffer_->G_albedo_.Raw();
        params->Output.load_op = RHILoadOpType::kDontCare;
        params->Output.store_op = RHIStoreOpType::kStore;
        params->View = view->view_common_params_;
        if (view->scene_ && view->scene_->GetSkyTexture() && view->scene_->GetSkyTexture()->GetDeviceTexture()) {
            params->SkyTexture = builder.Import(view->scene_->GetSkyTexture()->GetDeviceTexture());
        } else params->SkyTexture = nullptr; // Black
        params->LinearWrapSampler = RHI::Get().GetGlobalSamplers().linear_wrap;
    }
    builder.AddPass<SkyShader>(
        {}, shader, params,
        [shader, params](RDGPass *pass, RHICommandQueueGraphics &queue) {
            auto tid = pass->GetParameterTableId();
            RDGCommandHelper::Draw(queue, shader, tid,
                &SkyShader::GetShaderParamStructInfo()->render_pass_info_, params, 3);
        }
    );
}


MI_NAMESPACE_END