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
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

class DeviceWorld : public NonCopyable, public NonMovable {
public:
    // Index with renderable index.
    TRef<RHIBuffer> renderable_transforms_;

    // Record the index of the material of each geometry.
    TRef<RHIBuffer> renderable_geometry_material_indices_;
};

// Integrated class managing the world.
class World : public NonCopyable, public NonMovable {
public:
    // Create a static mesh renderable and add it to the world.
    // Releasing the reference yourself will remove it from the renderer.
    StaticMesh * CreateStaticMeshRenderable () ;

    void RemoveRenderable (Renderable * renderable) ;

    FORCEINLINE const std::vector<TRef<Renderable>> & GetRenderables () const {
        return renderables_;
    }

    FORCEINLINE DeviceWorld * GetDevice () const {
        return device_world_.get();
    }

protected:
    std::vector<TRef<Renderable>> renderables_;
    std::unique_ptr<DeviceWorld> device_world_;
};

MI_NAMESPACE_END

#endif //MI_WORLD_H
