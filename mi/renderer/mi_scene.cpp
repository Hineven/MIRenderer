/*
 * Created: 2025/4/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_scene.h"

#include "shaders/shared/SharedRenderable.hlsl"

#include <rhi/rhi.h>
#include <renderer/mi_static_mesh.h>
#include <renderer/mi_texture.h>
#include <rhi/rhi_buffer.h>
#include <rhi/rhi_as.h>

MI_NAMESPACE_BEGIN

DeviceScene::DeviceScene () {
    auto & rhi = RHI::Get();
    d_renderable_transforms_ = rhi.CreateBuffer(sizeof(glm::mat4x3) * Scene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage);
    d_renderable_transforms_->SetName("RenderableTransforms");
    d_renderable_inverse_transforms_ = rhi.CreateBuffer(sizeof(glm::mat4x3) * Scene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage);
    d_renderable_inverse_transforms_->SetName("RenderableInverseTransforms");
    d_renderable_normal_transforms_ = rhi.CreateBuffer(sizeof(glm::mat3x3) * Scene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage);
    d_renderable_normal_transforms_->SetName("RenderableNormalTransforms");
    d_renderable_headers_    = rhi.CreateBuffer(sizeof(RenderableHeader) * Scene::kMaxNumRenderables, RHIBufferUsageFlagBits::kStorage);
    d_renderable_headers_->SetName("RenderableHeaders");
    // Force first TLAS build
    tlas_instance_count_ = UINT32_MAX;
}

DeviceScene::~DeviceScene() {

}

Scene::Scene(): renderable_slots_(kMaxNumRenderables) {

}
Scene::~Scene() {

}



void Scene::SetSkyCube(Texture *texture) {
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


void Scene::RemoveRenderable (Renderable * renderable) {
    auto it = std::find_if(renderables_.begin(), renderables_.end(),
        [renderable](const TRef<Renderable> & r) { return r.Raw() == renderable; });
    if (it != renderables_.end()) {
        // Not remove from the list, but set to nullptr to keep the indices valid.
        it->SafeRelease();
    }
}

void Scene::CreateOnDevice() {
    if (!device_scene_) {
        device_scene_ = new DeviceScene();
        mi_check(device_scene_.IsValid(), "Failed to allocate device scene.");
    }
}

void Scene::UpdateAABB() {
    aabb_ = {};
    auto update = [&](glm::vec3 p, glm::mat4 transform) {
        auto world_pw = transform * glm::vec4(p, 1.0f);
        auto world_p = glm::vec3(world_pw.x, world_pw.y, world_pw.z) / world_pw.w;
        aabb_.min = glm::min(aabb_.min, world_p);
        aabb_.max = glm::max(aabb_.max, world_p);
    };
    for (const auto & renderable : renderables_) {
        if (renderable) {
            auto aabb = renderable->GetAABB();
            if (aabb.IsValid()) {
                auto transform = renderable->GetTransform().GetToWorldTransformMatrix();
                update(aabb.min, transform);
                update(aabb.max, transform);
                update(glm::vec3(aabb.min.x, aabb.min.y, aabb.max.z), transform);
                update(glm::vec3(aabb.min.x, aabb.max.y, aabb.min.z), transform);
                update(glm::vec3(aabb.min.x, aabb.max.y, aabb.max.z), transform);
                update(glm::vec3(aabb.max.x, aabb.min.y, aabb.min.z), transform);
                update(glm::vec3(aabb.max.x, aabb.min.y, aabb.max.z), transform);
                update(glm::vec3(aabb.max.x, aabb.max.y, aabb.min.z), transform);
            }
#ifndef NDEBUG
            if (glm::any(glm::isnan(aabb_.min)) || glm::any(glm::isnan(aabb_.max))) {
                MI_WARN("Scene AABB for renderable {} contains NaN values.", renderable->GetIndex());
            }
#endif
        }
    }
}


MI_NAMESPACE_END