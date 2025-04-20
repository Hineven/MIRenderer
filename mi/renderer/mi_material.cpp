/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_material.h"

#include <renderer/mi_renderer.h>

#include "core/infra.h"
#include "renderer/mi_resource_allocator.h"
MI_NAMESPACE_BEGIN
BindlessRendererTexture::BindlessRendererTexture(RenderResourceAllocator *allocator) {
    allocator_ = allocator;
    index_ = allocator->AllocateTextureIndex();
}


void BindlessRendererTexture::Set(RHITexture * texture) {
    assert(index_ != UINT32_MAX && allocator_);
    allocator_->SetTexture(index_, texture);
}

BindlessRendererTexture::~BindlessRendererTexture() {
    if (index_ != UINT32_MAX) {
        allocator_->ReleaseTextureIndex(index_);
    }
}

Material::Material(RenderResourceAllocator *allocator) {
    allocator_ = allocator;
}

Material::~Material() {
    
}



MI_NAMESPACE_END