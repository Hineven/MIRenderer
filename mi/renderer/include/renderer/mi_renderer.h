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

    void Init () ;
    // Called each frame
    void Render (RendererView * view_state, RenderGraphBuilder & builder) ;

protected:

    // A list of renderables. Ones with reference count approaching 1 will be removed from the list prior to frame
    std::vector<TRef<Renderable>> renderables_;
    // Vertex buffer pool
    std::vector<TRef<RHIBuffer>> vertex_buffers_;
    // Index buffer pool
    std::vector<TRef<RHIBuffer>> index_buffers_;

    // The renderer does not keep materials and textures itself (they are kept by the material classes),
    uint32_t top_texture_slot_ {};
    uint32_t top_material_slot_ {};
    std::stack<uint32_t> free_texture_slots_;
    std::stack<uint32_t> free_material_slots_;

    // Called by material / bindless texture destructor
    FORCEINLINE void ReleaseMaterialIndex (int index) {
        free_material_slots_.push(index);
    }
    FORCEINLINE void ReleaseTextureIndex (int index) {
        free_texture_slots_.push(index);
    }
    // Called by material / bindless texture constructor
    FORCEINLINE uint32_t AllocateMaterialIndex () {
        if(free_material_slots_.empty()) {
            return top_material_slot_++;
        } else {
            int index = free_material_slots_.top();
            free_material_slots_.pop();
            return index;
        }
    }
    FORCEINLINE uint32_t AllocateTextureIndex () {
        if(free_texture_slots_.empty()) {
            return top_texture_slot_++;
        } else {
            int index = free_texture_slots_.top();
            free_texture_slots_.pop();
            return index;
        }
    }
};


MI_NAMESPACE_END
#endif //MI_RENDERER_H
