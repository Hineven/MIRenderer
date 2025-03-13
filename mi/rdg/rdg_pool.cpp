/*
 * Created: 2025/3/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rdg/rdg_pool.h"

#include "rhi/rhi.h"
#include <rdg/rdg_resource.h>
MI_NAMESPACE_BEGIN
void RDGResourcePool::AllocateResource(RDGBuffer *buffer) {
    assert(!buffer->IsAllocated() && "This buffer should not be allocated already.");
    auto & RHI = RHI::Get();
    auto hash = buffer->GetResourceClassHash();
    auto & slot = rhi_free_buffer_map_[hash];
    RHIBuffer * allocated_buffer = nullptr;
    if (slot.empty()) {
        // Allocate a new buffer
        auto desc = buffer->GetDesc();
        auto rhi_buffer = RHI.CreateBuffer(desc);
        allocated_buffer = rhi_buffer.Raw();
        rhi_buffer_references_.emplace_back(std::move(rhi_buffer));
    }
    else {
        allocated_buffer = slot.back();
        slot.pop_back();
    }
}

void RDGResourcePool::RecycleResource(RDGBuffer *buffer) {
    auto hash = buffer->GetResourceClassHash();
    rhi_free_buffer_map_[hash].push_back(buffer->rhi_buffer_span_.buffer);
    buffer->rhi_buffer_span_ = {};
}

void RDGResourcePool::AllocateResource(RDGTexture *texture) {
    assert(!texture->IsAllocated() && "This texture should not be allocated already.");
    auto & RHI = RHI::Get();
    auto hash = texture->GetResourceClassHash();
    auto & slot = rhi_free_texture_map_[hash];
    RHITexture * allocated_texture = nullptr;
    if (slot.empty()) {
        // Allocate a new texture
        auto desc = texture->GetDesc();
        auto rhi_texture = RHI.CreateTexture(desc);
        allocated_texture = rhi_texture.Raw();
        rhi_texture_references_.emplace_back(std::move(rhi_texture));
    }
    else {
        allocated_texture = slot.back();
        slot.pop_back();
    }
}



MI_NAMESPACE_END