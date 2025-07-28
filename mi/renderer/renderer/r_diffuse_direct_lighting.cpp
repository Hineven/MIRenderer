/*
 * Created: 2025/7/25
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer.h"
MI_NAMESPACE_BEGIN

class ComputeDirectLightingShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, )
        SHADER_RESOURCE_PARAMETER(SamplerState, Sampler)
    END_SHADER_PARAMETERS()
};

void Renderer::Render_ComputeDirectLighting(RendererView *view, RenderGraphBuilder &builder) {
    // TODO light sampling structure construction


}


MI_NAMESPACE_END