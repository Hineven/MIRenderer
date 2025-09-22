/*
 * Created: 2025/9/22
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer.h"
#include "r_view_common.h"
#include "rdg/rdg_helper.h"
MI_NAMESPACE_BEGIN

class FilterDiffuseDirectIlluminationShader : public RDGShader {

};

IMPLEMENT_RDG_COMPUTE_SHADER(FilterDiffuseDirectIlluminationShader, "mi/renderer/shader/FilterDiffuseDirectIllumination.lsl", "FilterDiffuseDirectIllumination");

void Renderer::Render_DenoiseLighting(RendererView *view, RenderGraphBuilder &builder) {

}


MI_NAMESPACE_END