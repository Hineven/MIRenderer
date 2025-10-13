/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_VIEW_H
#define MI_RENDERER_VIEW_H

#include "rdg/rdg_fwd.h"
#include "mi_camera.h"
#include "mi_cvar.h"
#include "core/util/alloc.h"
#include "renderer/mi_renderer_fwd.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
MI_NAMESPACE_BEGIN
struct DebugCommonShaderParameters;

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

struct RendererViewPersistentData;
struct WorldRadianceCacheData;
struct LightStructureData;

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

    // Used to index the material indices buffer for geometries within the renderable using renderable index.
    TRef<RDGBuffer> static_mesh_geometry_material_indices_start_index;

    // Visibility buffer
    // 0: Renderable index, 1: Descriptor Index (8bits) + Primitive Index (24bits)
    // 2, 3: Barycentrics
    TRef<RDGTexture> G_visibility_;

    TRef<RDGTexture> G_depth_;
    TRef<RDGTexture> G_albedo_;
    TRef<RDGTexture> G_normal_;
    TRef<RDGTexture> G_emission_;
    TRef<RDGTexture> G_metallic_roughness_;

    TRef<RDGTexture> forward_depth_;

	TRef<RDGTexture> shadow_map_moments_;

    // Flags (R8Uint)
    TRef<RDGTexture> G_flags_;

    // Min-max values for the rendered volume segment
    TRef<RDGTexture> G_volume_min_max_;
    // Volume density
    TRef<RDGTexture> G_volume_density_;
    // Albedo of the volume segment
    TRef<RDGTexture> G_volume_color_;
    // Volume density in fourier term
    TRef<RDGTexture> G_volume_density_fourier_;
    // Albedo of the volume segment multiplied by density in fourier term
    TRef<RDGTexture> G_volume_weighted_color_fourier_;
    // CDF of recorded volume segment
    TRef<RDGTexture> G_volume_cdf_attenuation_;

    TRef<RDGTexture> G_transmittance_;

    TRef<RDGTexture> volume_sample_color_and_linear_depth_;
    TRef<RDGTexture> volume_sample_transmittance_and_pdf_;

    // HiZ buffer
    TRef<RDGTexture> hzb_;
    // or flags
    TRef<RDGTexture> or_flags_;

    // Shared world radiance cache data
    TRef<WorldRadianceCacheData> world_cache_;
    // Shared data for light sampling
    TRef<LightStructureData> light_structure_;

    // Diffuse direct lighting
    TRef<RDGTexture> diffuse_direct_lighting_;
    TRef<RDGTexture> denoised_diffuse_direct_lighting_;

    // Diffuse indirect lighting
    TRef<RDGTexture> diffuse_indirect_lighting_;
    // Special: this is set by the denoiser. Not created by the view itself.
    TRef<RDGTexture> denoised_diffuse_indirect_lighting_;

    // Volume direct lighting
    TRef<RDGTexture> volume_direct_lighting_;

    // Final radiance
    TRef<RDGTexture> radiance_;
    // Shaded radiance without emission, created & written by final composition
    TRef<RDGTexture> shaded_radiance_no_emission_;

    // Debug output, can be written to for debug purposes
    // This is tone mapped the same as radiance_
    TRef<RDGTexture> debug_output_;

    struct {
        // Outputs from debug paasses, can be used as we like
        TRef<RDGTexture> visualize_ray_tracing_scene_output_;
        TRef<RDGTexture> visualize_traced_rays_output_;
    } debug_views_;

    struct DebugBuffers {
        // For visualizing traced rays. Can be created and written to in various passes.
        TRef<RDGBuffer> traced_ray_count;
        TRef<RDGBuffer> traced_ray_origins;
        TRef<RDGBuffer> traced_ray_directions;
        TRef<RDGBuffer> traced_ray_states;
        TRef<RDGBuffer> traced_ray_colors;

        void CreateTracedRayBuffers (RenderGraphBuilder & builder, uint32_t max_num_rays);
    } debug_buffers_;

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
    void MakeSureHashGridPersistentDataExists (RenderGraphBuilder & builder);
};

// Used for setting cursor positions in debug uniform buffers
extern CVar<int> CVar_DebugCursorScreenCoordsX;
extern CVar<int> CVar_DebugCursorScreenCoordsY;

MI_NAMESPACE_END

#endif //MI_RENDERER_VIEW_H

