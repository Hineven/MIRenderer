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
MI_NAMESPACE_BEGIN
class RHIBuffer;
class RenderGraphBuilder;
class RHITexture;

struct Render_StaticMeshesData;

// Integrated with scene resource management... Maybe I'll separate it later
class Renderer : public NonCopyable, public NonMovable {
public:
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

    // 4 million at most
    constexpr static uint32_t kMaxNumActiveVolumePrimitives = 4 * 1024 * 1024;

protected:

    Renderer();
    ~Renderer();

    struct DrawInvocationSortingHeader {
        uint32_t material_index;
        uint32_t world_renderable_handle;
        RHIBuffer * vertex_buffer;
        RHIBuffer * index_buffer;
        RHIDrawIndexedIndirectCommand indirect_command;
    };

    void Render_DrawSky (RendererView * view, RenderGraphBuilder & builder) ;
    void Render_PrepareStaticMeshes (
        RendererView * view, RenderGraphBuilder & builder
    );
    void Render_DrawStaticMeshes (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_DrawVolumePrimitives (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_ComputeHiZBuffer (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_ComputeDirectLighting (
        RendererView * view, RenderGraphBuilder & builder
    ) ;

    void Render_LightingComposition (
        RendererView * view, RenderGraphBuilder & builder
    ) ;
    void Render_DrawToOutput (
        RendererView * view, RenderGraphBuilder & builder,
        RDGTexture * texture
    ) ;

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
        RDGBuffer * ray_to_trace_transmittance
    );

    // Render material properties from the camera using ray-tracing for debugging purposes.
    void Render_VisualizeRayTraced (RendererView * view, RenderGraphBuilder & builder) ;


    struct FrameContext {
        std::vector<TRef<Renderable>> visible_renderables;
        struct StaticMeshes {
            std::vector<DrawInvocationSortingHeader> draw_invocation_sorting_headers;
            std::vector<RHIDrawIndexedIndirectCommand> draw_indirect_commands;
            // Indirect draw commands (device side)
            TRef<RDGBuffer> d_static_draw_commands;
            // Used to index the renderable & material for draw commands, used for viewport rasterization
            TRef<RDGBuffer> d_static_mesh_draw_command_renderable_material_indices;
        } static_meshes;

        void Init ();
        void Deinit ();
    } ctx;

    TRef<DeviceBindlessResourceAllocator> device_allocator_;
    TRef<RDGResourcePool> pool_;

};


MI_NAMESPACE_END
#endif //MI_RENDERER_H
