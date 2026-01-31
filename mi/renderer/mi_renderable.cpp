/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_scene.h>
#include <renderer/mi_renderable.h>

MI_NAMESPACE_BEGIN
Renderable::Renderable(RenderableType type, Scene * scene): type_(type), scene_(scene) {
    index_keeper_ = scene->AllocateRenderableSlot();
    if (!IsValid()) {
        MI_LOG(MIInfraLogType::kError, "Failed to allocate renderable index from world."
                                       "Potentially too many renderables in the world.");
    }
}

RenderableHeader Renderable::GetDeviceRenderableHeader () const {
    return {
        {0, 0, 0, std::bit_cast<float>(GetRenderableFlags())}
    };
}

uint32_t Renderable::DecRef() {
    ref_count_--;
    if (ref_count_ == 0) {
        // Immediately unregister from the scene
        scene_->UnregisterRenderableAtIndex(GetIndex());
        delete this;
    }
    return ref_count_;
}

Renderable::~Renderable() {}

bool Renderable::IsEmpty() const {
    return false; // Default implementation, can be overridden
}


MI_NAMESPACE_END