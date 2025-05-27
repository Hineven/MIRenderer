/*
 * Created: 2025/4/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_scene.h"

#include "shaders/shared/SharedRenderable.hlsl"

#include <rhi/rhi.h>
#include <renderer/mi_helpers.h>
#include <renderer/mi_static_mesh.h>
#include <renderer/mi_texture.h>
#include <rhi/rhi_buffer.h>
#include <renderer/mi_buffer_heap.h>
#include <rhi/rhi_bindlesskeeper.h>

MI_NAMESPACE_BEGIN

RendererScene::RendererScene () {
    auto & rhi = RHI::Get();
    d_renderable_transforms_ = rhi.CreateBuffer(sizeof(glm::mat4x3) * RendererScene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage);
    d_renderable_headers_    = rhi.CreateBuffer(sizeof(RenderableHeader) * RendererScene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage);
    d_static_mesh_renderable_materials_ = DefaultDeviceBufferHeap::Create(RHIBufferUsageFlagBits::kStorage, 1, sizeof(uint32_t) * RendererScene::kMaxNumStaticMeshGeometryMaterialPairs);
    d_static_mesh_renderable_materials_->SetNumBufferBlockLimit(1);
    d_static_mesh_renderable_materials_->PreAllocateBlocks(1);
}

RendererScene::~RendererScene() {

}


void RendererScene::SetSkyCube(Texture *texture) {
    if (!texture) {
        sky_cube_ = nullptr;
        return ;
    }
    if (!texture->IsBindless()) {
        MI_WARN("Setting sky texture to a non-bindless texture will not take any effect.");
        return ;
    }
    sky_cube_ = texture;
}


void RendererScene::RemoveRenderable (Renderable * renderable) {
    auto it = std::find_if(renderables_.begin(), renderables_.end(),
        [renderable](const TRef<Renderable> & r) { return r.Raw() == renderable; });
    if (it != renderables_.end()) {
        renderables_.erase(it);
    }
}

MI_NAMESPACE_END