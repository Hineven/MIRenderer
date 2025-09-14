/*
 * Created: 2025/9/13
 * Author: Exploring Air Joe
 */

#ifndef EAJ_3D_VIEWER_H
#define EAJ_3D_VIEWER_H

#include "vulkan/vulkan.hpp"
#include <glfw/glfw3.h>
#include <imgui.h>

#include <rhi/vk/vk_export.h>
#include <rdg/rdg_pool.h>
#include <rhi/rhi_buffer.h>
#include <rhi/rhi_texture.h>

#include "infra_impl/infra.h"
#include "rhi/rhi_thread.h"
#include "rhi/rhi.h"
#include "rhi/rhi_cmd.h"
#include "rdg/rdg_shader.h"
#include "core/util/debug_prof.h"
#include "imgui_impl_glfw.h"
#include "core/task.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_renderer_view.h"
#include "util/texture_loader.h"
#include "util/gltf_loader.h"
#include "util/volprims_loader.h"

//在render_frame.cpp中多使用的
#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"

void RenderFrame (MI_NAMESPACE::RendererView * view_state, MI_NAMESPACE::RDGResourcePool * pool) ;

#endif //EAJ_3D_VIEWER_H
