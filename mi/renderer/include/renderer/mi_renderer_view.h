/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_VIEW_H
#define MI_RENDERER_VIEW_H

#include "rdg/rdg_fwd.h"
#include "mi_camera.h"
#include "core/util/alloc.h"
#include "renderer/mi_renderer_fwd.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
MI_NAMESPACE_BEGIN

class BatchedUploadContext : public NonCopyable, public NonMovable {
protected:
    BatchedUploadContext() = default;
    // Current manual staging buffer. Allocate sub-buffers for staging purposes from it within the frame.
    // A new one will be allocated if the current one ran out. Allocation may also not be necessarily
    // on the current buffer.
    TRef<RHIBuffer> manual_staging_buffer_;
    uint32_t manual_staging_buffer_top_ {};
    // Keep track of the total size of the staging buffer allocated
    size_t manual_staging_memory_footprint_ {};

    // Whether the batched uploads are fired or not.
    bool fired_ {};

    struct PendingUpload {
        std::span<const uint8_t> data;
        RHIBufferSpan dst_buffer;
    };

    struct PendingRDGUpload {
        std::span<const uint8_t> data;
        TRef<RDGBuffer> dst_buffer;
        size_t dst_offset {};
    };

    std::vector<PendingUpload> pending_uploads_;
    std::vector<PendingRDGUpload> pending_rdg_uploads_;
    std::vector<RDGBuffer * > extra_barriers_;

public:
    friend struct RendererView;

    constexpr static uint32_t kStagingBufferDefaultSize = 1024 * 1024 * 64; // 64MB

    // Allocate a staging buffer from the context for manual use
    RHIBufferSpan AllocateManualStagingBuffer(size_t size) ;

    // Add an upload to the context. The upload will be batched and fired at the late beginning of the frame
    // You should manually place barriers.
    void AddUnsafe (RHIBufferSpan buffer, const void * data, size_t size);
    // Add an upload to the context. The upload will be batched and fired at the late beginning of the frame
    void Add(RDGBuffer *buffer, const void * data, size_t size, size_t dst_offset = 0);
    // Add an extra pass write usage to the context. The usage will be added to the upload pass.
    void AddExtraBarrier(RDGBuffer * buffer);

    // Add an RDG upload pass. Close the context.
    void Fire (RenderGraphBuilder & builder);
    void Init ();

};

// The data kept across frames for a view.
struct RendererViewPersistentData {

    RendererViewPersistentData() ;
    ~RendererViewPersistentData() ;

    void Init ();
    void Update (RendererView * view);

    TRef<RDGTexture> prev_G_depth;
    TRef<RDGTexture> prev_G_albedo;
    TRef<RDGTexture> prev_G_normal;
    TRef<RDGTexture> prev_G_roughness;

    Camera prev_camera;
    uint32_t view_index {};
    uint32_t frame_index_ {};


    RendererScene * prev_world_;
};


// Holds all the states that a renderer uses to render a view of a frame.
struct RendererView {

    RendererView ();
    ~RendererView ();

    // Called once per frame to initialize the view.
    void InitFrame ();

    // Called once per frame at the frame end to roll data to the persistent store
    void UpdatePersistentData ();

    // Update view common shader parameters
    void SetViewCommonShaderParameters (RenderGraphBuilder & builder);

    Camera camera_ {};

    uint32_t film_width_ {};
    uint32_t film_height_ {};

    RendererScene * world_ {};

    // Used to index the material indices buffer for geometries within the renderable using renderable index.
    TRef<RDGBuffer> static_mesh_geometry_material_indices_start_index;

    TRef<RDGTexture> G_depth_;
    TRef<RDGTexture> G_albedo_;
    TRef<RDGTexture> G_normal_;
    TRef<RDGTexture> G_metallic_roughness_;

    TRef<RDGTexture> debug_output_;

    // Used for uploading data to the device on this frame. Batching small uploading calls for performance.
    BatchedUploadContext upload_context_;

    // Temporaries allocated for the frame
    TOneTimeLinearAllocator<> temp_allocator_;

    // Generated view common parameters (for this frame)
    ViewCommonShaderParameters * view_common_params_;

    // Persistent data
    std::unique_ptr<RendererViewPersistentData> persistent_data_ {};
};

MI_NAMESPACE_END

#endif //MI_RENDERER_VIEW_H

