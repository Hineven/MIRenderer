/*
 * Created: 2025/4/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_WORLD_H
#define MI_WORLD_H

#include <set>
#include <stack>
#include <vector>
#include "core/base.h"
#include "core/refcounted.h"
#include <rhi/rhi_fwd.h>

#include "mi_renderable.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

// Integrated class managing the rendering world. This class is not for general use and should only be used
// for rendering. Scene management is not its responsibility.
// It is responsible for holding renderables and rendering resources of a scene.
class RendererScene : public NonCopyable, public NonMovable {
public:
    friend class Renderable;
    friend struct RendererView;
    friend class Renderer;
    // TODO remove this
    friend class StaticMesh;

    constexpr static uint32_t kMaxNumRenderables = 4096;
    constexpr static uint32_t kMaxNumStaticMeshGeometryMaterialPairs = 4096 * 16;

    RendererScene();
    ~RendererScene();

    void RemoveRenderable (Renderable * renderable) ;

    FORCEINLINE const std::vector<TRef<Renderable>> & GetRenderables () const {
        return renderables_;
    }

    void SetSkyCube (Texture * texture) ;
    FORCEINLINE Texture * GetSkyTexture () const {
        return sky_cube_.Raw();
    }

protected:

    FORCEINLINE uint32_t AllocateRenderableIndex () {
        if (free_renderables_.empty()) {
            if (renderables_.size() < kMaxNumRenderables) {
                renderables_.emplace_back(nullptr);
                return (uint32_t)(renderables_.size() - 1);
            }
            return UINT32_MAX;
        }
        uint32_t index = free_renderables_.top();
        free_renderables_.pop();
        return index;

    }

    FORCEINLINE void FreeRenderabeIndex (uint32_t index) {
        free_renderables_.push(index);
    }

    std::vector<TRef<Renderable>> renderables_;
    // Keep track of free renderable indices, so we can reallocate them.
    std::stack<uint32_t> free_renderables_;

    TRef<Texture> sky_cube_;


    // Indexed with renderable index.
    TRef<RHIBuffer> d_renderable_transforms_;
    TRef<RHIBuffer> d_renderable_headers_;

    // Record the index of the material of each geometry from all static mesh renderables.
    // This buffer heap is limited to 1 buffer block. And it is always present.
    TRef<DeviceBufferHeapInterface> d_static_mesh_renderable_materials_;
};

MI_NAMESPACE_END

#endif //MI_WORLD_H
