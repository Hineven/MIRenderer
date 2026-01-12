/*
 * Created: 2025/4/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef INC_3D_VIEWER_H
#define INC_3D_VIEWER_H

#include <rdg/rdg_pool.h>

#include "rdg/rdg_builder.h"
#include "renderer/mi_renderer_view.h"

void RenderFrame (MI_NAMESPACE::RenderGraphBuilder & builder, MI_NAMESPACE::RendererView * view_state, bool render_scene);

#endif //INC_3D_VIEWER_H
