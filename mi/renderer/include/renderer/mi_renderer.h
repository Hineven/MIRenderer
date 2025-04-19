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

// Integrated with scene resource management... Maybe I'll separate it later
class Renderer : public NonCopyable, public NonMovable {
public:
    friend class BindlessRendererTexture;
    friend class Material;

    static Renderer & Get () ;
    static Renderer * GetPointer ();
    static void DestroySingleton () ;

    void Init (RDGResourcePool * pool) ;
    // Called each frame
    void Render (RendererView * view_state, RenderGraphBuilder & builder) ;

protected:

    void UpdateView (RendererView * view) ;

    // Draw a texture to back buffer directly.
    void Render_DrawToOutput (RendererView * view, RenderGraphBuilder & builder, RDGTexture * texture);

};


MI_NAMESPACE_END
#endif //MI_RENDERER_H
