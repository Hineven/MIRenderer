/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_scene.h>
#include <renderer/mi_renderable.h>

#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN
Renderable::Renderable(RenderableType type, Scene * scene): type_(type), scene_(scene) {
    index_keeper_ = scene->AllocateRenderableSlot();
    if (!IsValid()) {
        MI_LOG(MIInfraLogType::kError, "Failed to allocate renderable index from world."
                                       "Potentially too many renderables in the world.");
    } else {
        // Register renderable to the scene
        scene->RegisterRenderableAtIndex(GetIndex(), this);
    }
}

RenderableHeader Renderable::GetDeviceRenderableHeader () const {
    return {
        {0, 0, 0, std::bit_cast<float>(GetRenderableFlags())}
    };
}

void Renderable::QueueForDestruction() const {
    // Immediately unregister from the scene, making it invisible to the renderer.
    scene_->UnregisterRenderableAtIndex(GetIndex());
    // Queue up for destruction later
    scene_->EnqueueForDelayedDestruction(const_cast<Renderable *>(this));
}

Renderable::~Renderable() {
    // Noop
}

bool Renderable::IsEmpty() const {
    return false; // Default implementation, can be overridden
}

MI_NAMESPACE_END