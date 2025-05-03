/*
 * Created: 2025/4/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_world.h"

#include "shaders/SharedRenderable.hlsl"

#include <rhi/rhi.h>
#include <renderer/mi_helpers.h>
#include <renderer/mi_static_mesh.h>
#include <rhi/rhi_buffer.h>
#include <renderer/mi_buffer_heap.h>

MI_NAMESPACE_BEGIN

DeviceWorld::DeviceWorld () {
    auto & rhi = RHI::Get();
    renderable_transforms_ = rhi.CreateBuffer(sizeof(glm::mat4x3) * World::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage);
    renderable_headers_    = rhi.CreateBuffer(sizeof(RenderableHeader) * World::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage);
    static_mesh_renderable_materials_ = rhi.CreateBuffer(sizeof(uint32_t) * World::kMaxNumStaticMeshGeometryMaterialPairs, RHIBufferUsageFlagBits::kStorage);
}

void World::RemoveRenderable (Renderable * renderable) {
    auto it = std::find_if(renderables_.begin(), renderables_.end(),
        [renderable](const TRef<Renderable> & r) { return r.Raw() == renderable; });
    if (it != renderables_.end()) {
        renderables_.erase(it);
    }
}

MI_NAMESPACE_END