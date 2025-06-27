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

    void Init (CommonGroupedDeviceResourceAllocator * allocator, RDGResourcePool * pool) ;
    // Called each frame
    void Render (RendererView * view_state, RenderGraphBuilder & builder) ;

    FORCEINLINE CommonGroupedDeviceResourceAllocator * GetDeviceAllocator () {
        return device_allocator_.Raw();
    }

protected:

    Renderer();
    ~Renderer();

    struct DrawInvocationSortingHeader {
        uint32_t material_index;
        uint32_t world_renderable_handle;
        RHIBufferSpan vertex_buffer;
        RHIBufferSpan index_buffer;
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
    void Render_DrawToOutput (RendererView * view, RenderGraphBuilder & builder, RDGTexture * texture) ;


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

    TRef<CommonGroupedDeviceResourceAllocator> device_allocator_;
    TRef<RDGResourcePool> pool_;

};


MI_NAMESPACE_END
#endif //MI_RENDERER_H
