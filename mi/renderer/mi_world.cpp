/*
 * Created: 2025/4/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_world.h"

#include <rhi/rhi.h>
#include <renderer/mi_helpers.h>
#include <renderer/mi_static_mesh.h>

MI_NAMESPACE_BEGIN

StaticMesh * World::CreateStaticMeshRenderable () {
    auto static_mesh = new StaticMesh();
    renderables_.emplace_back(static_mesh);
    return static_mesh;
}

void World::RemoveRenderable (Renderable * renderable) {
    auto it = std::find_if(renderables_.begin(), renderables_.end(),
        [renderable](const TRef<Renderable> & r) { return r.Raw() == renderable; });
    if (it != renderables_.end()) {
        renderables_.erase(it);
    }
}

MI_NAMESPACE_END