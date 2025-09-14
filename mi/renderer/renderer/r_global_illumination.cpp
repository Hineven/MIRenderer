/*
 * Created: 2025/9/10
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_shader.h"
#include "rdg/rdg_builder.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer.h"
#include "r_view_common.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_resource_allocator.h"
#include "../shaders/shared/SharedLight.hlsl"
#include "renderer/mi_scene.h"


MI_NAMESPACE_BEGIN

void Renderer::Render_DiffuseIndirectIllumination(RendererView *view, RenderGraphBuilder &builder) {
    // Spawn probes
    // Reproject probes
    //
}