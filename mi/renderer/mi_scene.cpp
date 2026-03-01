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
    mi_check_nothrow(!renderable_slots_.NoAllocationActive(),
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

void Scene::CreateOnDevice() {
    if (!device_scene_) {
        device_scene_ = new DeviceScene();
        mi_check(device_scene_.IsValid(), "Failed to allocate device scene.");
    }
}

TRef<TDelayedReleaseKeeper<Scene>> Scene::AllocateRenderableSlot () {
    auto slot = renderable_slots_.AllocateSlot();
    if (slot == UINT32_MAX) return {};
    auto keeper = Create<TDelayedReleaseKeeper<Scene>>(this, slot, [](Scene * owner, uint32_t index) {
        owner->FreeRenderabeIndex(index);
    });
    return std::move(keeper);
}

void Scene::UpdateAABB() {
    aabb_ = {};
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

void Scene::EnqueueForDelayedDestruction(DelayedDestructionResource *obj) {
    delayed_destruction_.Enqueue(obj);
}

void Scene::AdvanceFrameForDelayedDestruction() {
    delayed_destruction_.Tick();
}

void Scene::ForceFlushDelayedDestruction() {
    delayed_destruction_.ClearAllNow();
}


MI_NAMESPACE_END