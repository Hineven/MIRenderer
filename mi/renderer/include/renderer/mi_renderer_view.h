/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_VIEW_H
#define MI_RENDERER_VIEW_H

#include <core/util/alloc.h>
#include <rhi/rhi_fwd.h>
#include <rhi/rhi_desc.h>
#include <rdg/rdg_fwd.h>
#include <renderer/mi_renderer_fwd.h>
#include <renderer/mi_camera.h>
#include <renderer/mi_cvar.h>

#include "mi_aabb.h"
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
    // Add an extra kTransferWrite usage to the context. The usage will be added to the upload pass.
    void AddExtraBarrier(RDGBuffer * buffer);

    // Add an RDG upload pass. Close the context.
    void Fire (RenderGraphBuilder & builder);
    void Init ();

};

// Holds all the states that a renderer uses to render a view of a frame.
struct RendererView {

    RendererView ();
    ~RendererView ();

    // Called once per frame to initialize the view.
    void InitFrame ();

    // Update view common shader parameters
    void SetupViewCommonShaderParameters (RenderGraphBuilder & builder);
    // Update debug common shader parameters
    void SetupDebugCommonShaderParameters (RenderGraphBuilder & builder);

    Camera camera_ {};

    uint32_t film_width_ {};
    uint32_t film_height_ {};

    Scene * scene_ {};

    // Geometry buffers, rendered by deferred passes & some special passes
    TRef<GeometryBufferData> g_buffer_;

    // Depth for forward rendering pass
    TRef<RDGTexture> forward_depth_;
    // Shadow map (d, blurred d2) for the main directional light
	TRef<RDGTexture> shadow_map_moments_;

    // HiZ buffer
    TRef<RDGTexture> hzb_;
    // Mipmapped G_flags_ using bitwise OR, has the same dimensions as hzb_
    TRef<RDGTexture> or_flags_;

    // Shared world radiance cache data
    TRef<WorldRadianceCacheData> world_cache_;
    // Shared data for light sampling
    TRef<LightStructureData> light_structure_;
    // Shared data from diffuse direct lighting (mesh)
    TRef<DiffuseDirectLightingData> diffuse_direct_lighting_;
    // Shared data from volume grid direct lighting
    TRef<VolumeGridDirectLightingData> volume_grid_direct_lighting_;
    // Shared data from diffuse indirect lighting (mesh)
    TRef<DiffuseIndirectLightingData> diffuse_indirect_lighting_;
    // Shared data from denoiser
    TRef<DenoiserViewData> denoiser_;

    // Final radiance (after composition)
    TRef<RDGTexture> radiance_;
    // TAA-resolved radiance (after temporal accumulation)
    TRef<RDGTexture> taa_radiance_;
    // Linear color overlay for gaussian RDF / forward passes (kR8G8B8A8_UNORM)
    TRef<RDGTexture> overlay_;
    // Shaded radiance without emission, created & written by final composition, used for lighting reuse
    TRef<RDGTexture> shaded_radiance_no_emission_;
    // Shaded volume radiance, created & written by final composition, used for lighting reuse
    TRef<RDGTexture> shaded_volume_radiance_;

    // Debug output, can be written to for debug purposes
    // This is tone mapped the same as radiance_
    TRef<RDGTexture> debug_output_;

    struct {
        // Outputs from debug paasses, can be used as we like
        TRef<RDGTexture> visualize_ray_tracing_scene_output_;
        TRef<RDGTexture> visualize_traced_rays_output_;
        TRef<RDGTexture> visualize_world_cache_output_;
        TRef<RDGTexture> visualize_spatial_positions_output_;
    } debug_views_;

    struct DebugBuffers {
        // For visualizing traced rays. Can be created and written to in various passes.
        TRef<RDGBuffer> traced_ray_count;
        TRef<RDGBuffer> traced_ray_origins;
        TRef<RDGBuffer> traced_ray_directions;
        TRef<RDGBuffer> traced_ray_states;
        TRef<RDGBuffer> traced_ray_colors;

        // Visualizing a series of spatial positions in the scene
        TRef<RDGBuffer> visualize_spatial_positions_count;
        TRef<RDGBuffer> visualize_spatial_positions;

        void CreateTracedRayBuffers (RenderGraphBuilder & builder, uint32_t max_num_rays);
        void CreateVisualizeSpatialPositionsBuffers (RenderGraphBuilder & builder, uint32_t max_num_positions);
    } debug_buffers_;

    struct ShadowMappingData {
        // If true, use the whole scene bounds as mapping_world_bounds_.
        // Otherwise, use the existing mapping_world_bounds_.
        bool use_world_bounds_ {true};
        AABB mapping_world_bounds_ {};
        glm::mat4x4 light_world_to_ndc_ {};
    } shadow_mapping_;

    // Used for uploading data to the device on this frame. Batching small uploading calls for performance.
    BatchedUploadContext upload_context_;

    // Temporaries allocated for the frame
    TOneTimeLinearAllocator<> temp_allocator_;

    // Generated view common parameters (for this frame)
    ViewCommonShaderParameters * view_common_params_;

    // Common parameters may be useful in debugging
    DebugCommonShaderParameters * debug_common_params_;

    // Persistent data
    RendererViewPersistentData * persistent_data_ {};
    void CreateSharedResources (RenderGraphBuilder & builder, bool should_render_volume_lighting = true);
    void MakeSurePersistentDataExists (RenderGraphBuilder & builder);

    // Current frame jitter (NDC space per-axis)
    glm::vec2 camera_jitter_ {};

    // Whether path tracing has already been scheduled for this frame.
    bool did_render_path_tracing_this_frame_ {false};
};

// Used for setting cursor positions in debug uniform buffers
extern CVar<int> CVar_DebugCursorScreenCoordsX;
extern CVar<int> CVar_DebugCursorScreenCoordsY;

MI_NAMESPACE_END

#endif //MI_RENDERER_VIEW_H

