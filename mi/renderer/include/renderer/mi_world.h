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
#include <rhi/rhi_texture.h>
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

// Integrated class managing the world.
class World : public NonCopyable, public NonMovable {
public:

    // Create a static mesh renderable and add it to the world.
    // Releasing the reference yourself will remove it from the renderer.
    TRef<StaticMesh> CreateStaticMeshRenderable () ;
protected:

};

MI_NAMESPACE_END

#endif //MI_WORLD_H
