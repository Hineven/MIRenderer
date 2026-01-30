/*
 * Created: 2025/4/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_H
#define MI_RENDERER_H

#include <stack>
#include <vector>

#include "core/base.h"
#include "core/common.h"
#include "core/refcounted.h"
#include "rhi/rhi_desc.h"
#include "rdg/rdg_base.h"
#include "renderer/mi_renderer_fwd.h"
#include "renderer/mi_renderer_view.h"
#include "renderer/mi_camera.h"
#include "renderer/mi_console.h"
MI_NAMESPACE_BEGIN
class RHIBuffer;
class RenderGraphBuilder;
class RHITexture;

struct Render_StaticMeshesData;

enum class PostProcessingFlagBits : unsigned {
    eNone = 0,
    // Use TAA (using visibility buffer and motion vectors)
    eEnableTAA = 1 << 0,
};
MAKE_FLAGS(PostProcessing);

// Integrated with scene resource management... Maybe I'll separate it later
class Renderer : public NonCopyable, public NonMovable {
public:

    enum class DrawToOutputMappingType : unsigned {
        eRadianceToSRGB = 0,
        eLinearToSRGB
    };

    friend class BindlessRendererTexture;
    friend class Material;

    static Renderer & Get () ;
    static Renderer * GetPointer ();
    static void DestroySingleton () ;

    void Init (DeviceBindlessResourceAllocator * allocator, RDGResourcePool * pool) ;

    // Called each frame
    void Render (RendererView * view_state, RenderGraphBuilder & builder) ;

    FORCEINLINE DeviceBindlessResourceAllocator * GetDeviceAllocator () {
        return device_allocator_.Raw();
    }

    Console& GetConsole() { return console_; }

    // at most 4M
    constexpr static uint32_t kMaxNumActiveVolumePrimitives = 4 * 1024 * 1024;
    // at most 16M
    constexpr static uint32_t kMaxNumVolumePrimitiveInstances = 16 * 1024 * 1024;

    // Default shadow map resolution
    constexpr static uint32_t kDefaultShadowMapResolution = 1024;

protected:

    Renderer();
    ~Renderer();

    struct DrawInvocationSortingHeader {
        uint32_t descriptor_index;
        uint32_t world_renderable_handle;
        RHIBuffer * vertex_buffer;
        RHIBuffer * index_buffer;
        RHIDrawIndexedIndirectCommand indirect_command;
    };

    void Render_DrawSky (RendererView * view, RenderGraphBuilder & builder) ;
    void Render_PrepareStaticMeshes (
        RendererView * view, RenderGraphBuilder & builder
    );
    void Render_DrawDeferredStaticMeshes (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_DrawVolumePrimitives (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    void Render_DrawShadowMap (
        RendererView* view, RenderGraphBuilder& builder
    );

    void Render_ComputeHiZBuffer (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    // Reset light structure history (when a reset is needed)
    void Render_PrepareLightStructureHistory (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    // Update light structure history for temporal reuse, usually at the end of frames
    void Render_UpdateLightStructureHistory (
        RendererView * view, RenderGraphBuilder & builder
    );

    // Compute direct lighting
    void Render_ComputeDiffuseDirectLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_ComputeVolumeDirectLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_ComputeVolumeGridDirectLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    void Render_PrepareHashGridCache ( // Called by Render_ComputeIndirectDiffuseLighting()
        RendererView * view, RenderGraphBuilder & builder
    );
    void Render_UpdateDiffuseIndirectLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_UpdateVolumeIndirectLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_UpdateHashGridCache ( // Called by Render_ComputeIndirectDiffuseLighting()
        RendererView * view, RenderGraphBuilder & builder
    );
    void Render_FinishDiffuseIndirectLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_FinishVolumeIndirectLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    void Render_DenoiseLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    void Render_LightingComposition (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_DrawToOutput (
        RendererView * view, RenderGraphBuilder & builder,
        RDGTexture * texture,
        DrawToOutputMappingType mapping_type = DrawToOutputMappingType::eRadianceToSRGB,
        PostProcessingFlags post_processing_flags = PostProcessingFlagBits::eNone
    ) ;

    void Render_DrawForwardStaticMeshes (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    void Render_PathTracing (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    void Render_DebugView (
        RendererView * view, RenderGraphBuilder & builder
    );

    void Render_HardwareShadowRayTracing (
        RendererView * view, RenderGraphBuilder & builder,
        RDGBuffer * ray_to_trace_list_length,
        RDGBuffer * ray_to_trace_list,
        RDGBuffer * ray_to_trace_direction,
        RDGBuffer * ray_to_trace_state,
        // Either ray_to_trace_origin_screen_coords or ray_to_trace_origin should be used. The
        // other one should be nullptr.
        RDGBuffer * ray_to_trace_origin_screen_coords,
        RDGBuffer * ray_to_trace_origin,
        RDGBuffer * ray_to_trace_tmax
    );

    void Render_HardwareTransmittanceRayTracing (
        RendererView * view, RenderGraphBuilder & builder,
        RDGBuffer * ray_to_trace_list_length,
        RDGBuffer * ray_to_trace_list,
        RDGBuffer * ray_to_trace_direction,
        RDGBuffer * ray_to_trace_state,
        // Either ray_to_trace_origin_screen_coords or ray_to_trace_origin should be used. The
        // other one should be nullptr.
        RDGBuffer * ray_to_trace_origin_screen_coords,
        RDGBuffer * ray_to_trace_origin,
        RDGBuffer * ray_to_trace_tmax,
        RDGBuffer * ray_to_trace_transmittance,
        uint32_t seed = 0
    );

    void Render_HardwareRadianceRayTracing (
        RendererView * view, RenderGraphBuilder & builder,
        RDGBuffer * ray_to_trace_list_length,
        RDGBuffer * ray_to_trace_list,
        RDGBuffer * ray_to_trace_direction,
        RDGBuffer * ray_to_trace_state,
        // Either ray_to_trace_origin_screen_coords or ray_to_trace_origin should be used. The
        // other one should be nullptr.
        RDGBuffer * ray_to_trace_origin_screen_coords,
        RDGBuffer * ray_to_trace_origin,
        RDGBuffer * ray_to_trace_tmax,
        RDGBuffer * ray_to_trace_result, // Output fp16x4 (rgb, thit)
        uint32_t seed
    );

    void Render_HardwareVisibilityRayTracing (
        RendererView * view, RenderGraphBuilder & builder,
        RDGBuffer * ray_to_trace_list_length,
        RDGBuffer * ray_to_trace_list,
        RDGBuffer * ray_to_trace_direction,
        RDGBuffer * ray_to_trace_state,
        // Either ray_to_trace_origin_screen_coords or ray_to_trace_origin should be used. The
        // other one should be nullptr.
        RDGBuffer * ray_to_trace_origin_screen_coords,
        RDGBuffer * ray_to_trace_origin,
        RDGBuffer * ray_to_trace_tmax,
        // For normal tracing: uint2 (packed normal, packed cached material)
        // For full tracing: uint4 (full visibility)
        RDGBuffer * ray_to_trace_result,
        uint32_t seed, VisibilityTraceType trace_type
    );

    void Render_DrawGaussianRadianceFields (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    void Render_PrepareGaussianRadianceFields(RendererView * view, RenderGraphBuilder & builder);

    struct FrameContext {
        std::vector<TRef<Renderable>> visible_renderables;
        struct StaticMeshes {
            std::vector<DrawInvocationSortingHeader> draw_invocation_sorting_headers;
            std::vector<RHIDrawIndexedIndirectCommand> draw_indirect_commands;
            // Indirect draw commands (device side)
            TRef<RDGBuffer> d_static_draw_commands;
            // Used to index the renderable & material for draw commands, used for viewport rasterization
            TRef<RDGBuffer> d_static_mesh_draw_command_renderable_descriptor_indices;
        } deferred_static_meshes, forward_static_meshes;

        struct GaussianRadianceFields {
            std::vector<RHIDrawIndirectCommand> draw_indirect_commands; // one per instance for Filter pass
            TRef<RDGBuffer> d_filter_draw_commands; // uploaded indirect commands
            TRef<RDGBuffer> d_active_renderable_list_buffer; // ActiveGaussianRenderableListBuffer
            TRef<RDGBuffer> d_active_renderable_count_buffer; // ActiveGaussianRenderableCount

            std::vector<uint32_t> active_renderable_indices; // host side copy for preparing active lists
            uint32_t active_renderable_count;
        } gaussian_radiance_fields;

        void Init ();
        void Deinit ();
    } ctx;

    TRef<DeviceBindlessResourceAllocator> device_allocator_;
    TRef<RDGResourcePool> pool_;

    // Sampling related resources
    TRef<RHITexture> blue_noise_128x128_;
    TRef<RHIBuffer> sobol_256x256_;
    TRef<RHIBuffer> sobol_scrambling_tile_256x256x8_;

    Console console_;

};


MI_NAMESPACE_END
#endif //MI_RENDERER_H
