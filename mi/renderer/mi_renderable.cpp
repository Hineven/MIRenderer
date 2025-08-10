/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_renderable.h"

#include "renderer/mi_scene.h"
MI_NAMESPACE_BEGIN
Renderable::Renderable(RenderableType type, Scene * scene): type_(type), scene_(scene) {
    index_ = scene->AllocateRenderableIndex(this);
    if (!IsValid()) {
        MI_LOG(MIInfraLogType::kError, "Failed to allocate renderable index from world."
                                       "Potentially too many renderables in the world.");
    }
}

RenderableHeader Renderable::GetDeviceRenderableHeader () const {
    return {};
}

Renderable::~Renderable() {
    if (index_ != UINT32_MAX) {
        scene_->FreeRenderabeIndex(index_);
    }
}

bool Renderable::IsEmpty() const {
    return false; // Default implementation, can be overridden
}


MI_NAMESPACE_END