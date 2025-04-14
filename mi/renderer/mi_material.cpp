/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_material.h"

#include <renderer/mi_renderer.h>

#include "core/infra.h"
MI_NAMESPACE_BEGIN
void BindlessRendererTexture::Set(TRef<RHITexture> texture) {
    texture_ = texture;
    if (index_ == UINT32_MAX) {
        if (auto ptr = Renderer::GetPointer()) index_ = ptr->AllocateTextureIndex();
        else
            MI_WARN("Renderer is not initialized!");
    }
}

BindlessRendererTexture::~BindlessRendererTexture() {
    if (index_ != UINT32_MAX) {
        if (auto ptr = Renderer::GetPointer()) ptr->ReleaseTextureIndex(index_);
        else
            MI_WARN("Renderer is freed!");

    }
}

Material::Material() {
    if (index_ == UINT32_MAX) {
        if (auto ptr = Renderer::GetPointer()) index_ = ptr->AllocateMaterialIndex();
        else
            MI_WARN("Renderer is not initialized!");
    }
}

Material::~Material() {
    if (index_ != UINT32_MAX) {
        if (auto ptr = Renderer::GetPointer()) ptr->ReleaseMaterialIndex(index_);
        else
            MI_WARN("Renderer is freed!");
    }
}

MI_NAMESPACE_END