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
    mi_check(!renderable_slots_.NoAllocationActive(),
        "There are still renderables allocated from the scene during its destruction."
        "Make sure to purge all references to renderables before destroying the scene.");
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
    // Just use the renderable index to find and remove it.
    if (!renderable) return;
    auto index = renderable->GetIndex();
    if (index != UINT32_MAX && index < renderables_.size()) {
        if (renderables_[index] == renderable) {
            RemoveRenderableAtIndex(index);
        } else {
            mi_warning(false, "This renderable seems to not belong to this scene / already removed from this scene. Removal skipped.");
        }
    }
}

void Scene::RemoveRenderableAtIndex(uint32_t index) {
    if (index >= renderables_.size()) return;
    {
        if (renderables_[index]) {
            // Delay 1 frame before removal
            removing_renderables_[removing_renderables_list_index_ % 2].push_back(renderables_[index].Raw());
            renderables_[index].rat();asdfasdf
        }
    }
}

void Scene::FlushRemovingRenderables() {
    // TODO Make a super class to group resources that require delayed removal at renderer level.
    // for example: UberBuffers, renderable indices, material indices, ...etc.
    // Release the renderables that have been delayed for removal
    auto & to_remove = removing_renderables_[((removing_renderables_list_index_ + 1) % 2)];
    for (auto r : to_remove) {
        auto index = r->GetIndex();
        if (index != UINT32_MAX) {
            // Recycle the index of the renderable
            FreeRenderabeIndex(index);
            // Reset the index of the renderable: it no longer belongs to the scene
            r->index_ = UINT32_MAX;
        }
    }
    // Release references. This can potentially destroy the renderables (however, it's safe to do so here because of the delay).
    to_remove.clear();
    removing_renderables_list_index_++;
}

void Scene::CreateOnDevice() {
    if (!device_scene_) {
        device_scene_ = new DeviceScene();
        mi_check(device_scene_.IsValid(), "Failed to allocate device scene.");
    }
}

uint32_t Scene::AllocateRenderableIndexAndHash(Renderable *renderable) {
    auto slot = renderable_slots_.AllocateSlot();
    if (slot == UINT32_MAX) return UINT32_MAX;
    if (renderables_.size() <= slot) {
        renderables_.resize(slot + 1);
    }
    renderables_[slot] = renderable;
    renderable->hash_ = renderable_hash_generator();
    return slot;
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
                aabb_.Encapsulate(aabb.Transformed(transform));
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