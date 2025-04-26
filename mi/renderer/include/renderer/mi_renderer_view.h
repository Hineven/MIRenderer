/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_VIEW_H
#define MI_RENDERER_VIEW_H

#include "rdg/rdg_fwd.h"
#include "mi_camera.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN


// The data kept across frames for a view.
struct RendererViewPersistentData {
    // If the view is initialized. If not, initialization will be done
    // upon Renderer::UpdateView()
    bool initialized;

    TRef<RDGTexture> prev_G_depth;
    TRef<RDGTexture> prev_G_albedo;
    TRef<RDGTexture> prev_G_normal;
    TRef<RDGTexture> prev_G_roughness;

    Camera prev_camera;
    uint32_t view_index {};
    uint32_t frame_index_ {};
};

// Holds all the states that a renderer uses to render a view of a frame.
struct RendererView {



    Camera camera_;

    uint32_t film_width_ {};
    uint32_t film_height_ {};

    World * world_;

    TRef<RDGBuffer> static_mesh_draw_commands_;

    TRef<RDGTexture> G_depth_;
    TRef<RDGTexture> G_albedo_;
    TRef<RDGTexture> G_normal_;
    TRef<RDGTexture> G_roughness_;

    // Imported back buffer for current frame
    TRef<RDGTexture> output;

    RendererViewPersistentData * persistent_data_;
};

MI_NAMESPACE_END

#endif //MI_RENDERER_VIEW_H

