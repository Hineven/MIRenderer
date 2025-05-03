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
#include "rhi/rhi_fwd.h"
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

    // Used to index the material indices buffer for geometries within the renderable using renderable index.
    TRef<RDGBuffer> static_mesh_geometry_material_indices_start_index;
    uint32_t static_mesh_geometry_material_index_top {};
    TRef<RDGBuffer> static_mesh_geometry_material_indices_;

    TRef<RDGTexture> G_depth_;
    TRef<RDGTexture> G_albedo_;
    TRef<RDGTexture> G_normal_;
    TRef<RDGTexture> G_roughness_;

    // Imported back buffer for current frame
    TRef<RDGTexture> output;

    TRef<RDGBuffer> renderer_view;

    // Current staging buffer. Allocate sub-buffers for staging purposes from it within the frame.
    // A new one will be allocated if the current one ran out. Allocation may also not be necessarily
    // on the current buffer.
    TRef<RHIBuffer> staging_buffer_;
    uint32_t staging_buffer_top_;
    constexpr static uint32_t kStagingBufferDefaultSize = 1024 * 1024 * 64; // 64MB
    // Keep track of the total size of the staging buffer allocated
    uint32_t staging_memory_footprint_ {};
    RHIBufferSpan AllocateStagingBuffer(size_t size) ;

    RendererViewPersistentData * persistent_data_;
};

MI_NAMESPACE_END

#endif //MI_RENDERER_VIEW_H

