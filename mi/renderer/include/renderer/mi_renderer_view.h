/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_VIEW_H
#define MI_RENDERER_VIEW_H

#include "mi_camera.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN
class RDGTexture;
class RDGBuffer;
class RDGPool;

// Holds all the states that a renderer uses to render a view of a frame.
struct RendererView {
    RendererView (uint32_t width, uint32_t height, World * world);
    ~RendererView();


    uint32_t frame_index_ {};

    Camera camera_, prev_camera_;

    uint32_t film_width_ {};
    uint32_t film_height_ {};

    uint32_t view_index_ {};

    World * world_;

    TRef<RDGBuffer> static_mesh_draw_commands_;

    TRef<RDGTexture> G_depth_;
    TRef<RDGTexture> G_albedo_;
    TRef<RDGTexture> G_normal_;
    TRef<RDGTexture> G_roughness_;

    TRef<RDGTexture> prev_G_depth_;
    TRef<RDGTexture> prev_G_albedo_;
    TRef<RDGTexture> prev_G_normal_;
    TRef<RHITexture> prev_G_roughness_;

    // Imported back buffer
    TRef<RDGTexture> output_;
};

MI_NAMESPACE_END

#endif //MI_RENDERER_VIEW_H

