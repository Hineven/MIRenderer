/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_renderable.h"

#include "renderer/mi_scene.h"
MI_NAMESPACE_BEGIN
Renderable::Renderable(RenderableType type, uint32_t index, Scene * world): type_(type), index_(index), scene_(world) {

}

uint32_t Renderable::AllocateRenderableIndexFromWorld (Scene * world) {
    return world->AllocateRenderableIndex();
}

RenderableHeader Renderable::GetDeviceRenderableHeader () const {
    return {};
}

Renderable::~Renderable() {
    if (index_ != UINT32_MAX) {
        scene_->FreeRenderabeIndex(index_);
    }
}

void Renderable::RegisterToWorld() {
    scene_->renderables_[index_] = this;
}



MI_NAMESPACE_END