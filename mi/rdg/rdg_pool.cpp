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
    // TODO better strategy. Now I'll only implement a simple one
    assert(!buffer->IsAllocated() && "This buffer should not be allocated already.");
    assert(buffer->desc_.size > 0 && "Buffer size should be greater than 0.");
    auto & RHI = RHI::Get();
    RHIBuffer * allocated_buffer = nullptr;
    size_t requested_size = buffer->GetSize();
    if (buffer->dedicated_) {
        // Allocate a new buffer
        auto desc = buffer->GetDesc();
        auto rhi_buffer = RHI.CreateBuffer(desc);
        // Round up the size for alignment requirement
        desc.size = RoundUp(desc.size, RDGBuffer::kMinBufferSize);
        allocated_buffer = rhi_buffer.Raw();
        rhi_buffer_references_.emplace_back(std::move(rhi_buffer));
    } else {
        if (buffer->desc_.size <= kBufferBlockSize) {
            // Running out, find a larger one.
            auto desc = buffer->GetDesc();
            for (int i = RDGBuffer::kMinBufferSizeLog2;
                i < kBufferReusingAbsoluteThresholdLog2; i++) {
                size_t block_size = 1ull << i;
                if (block_size < requested_size) continue;
                desc.size = block_size;
                int ratio = i - (int)log2(requested_size);
                if (ratio > kBufferReusingThresholdLog2) {
                    // Too large to reuse, stop searching
                    break;
                }
                auto hash = RDGBuffer::GetResourceClassHash(desc, false);
                auto & slot = rhi_free_buffer_map_[hash];
                if (!slot.empty()) {
                    // Found one, allocate it
                    allocated_buffer = slot.back();
                    slot.pop_back();
                    break;
                }
            }
            // Pool memory ran out, allocate a new buffer block
            if (!allocated_buffer) {
                desc.size = kBufferBlockSize;
                auto rhi_buffer = RHI.CreateBuffer(desc);
                allocated_buffer = rhi_buffer.Raw();
                rhi_buffer_references_.emplace_back(std::move(rhi_buffer));
            }
        } else {
            // Use dedicated allocation for buffers larger than buffer block
            auto desc = buffer->GetDesc();
            // Round up the size for alignment requirement
            desc.size = RoundUp(desc.size, RDGBuffer::kMinBufferSize);
            auto rhi_buffer = RHI.CreateBuffer(desc);
            allocated_buffer = rhi_buffer.Raw();
            rhi_buffer_references_.emplace_back(std::move(rhi_buffer));
        }
    }
    // Try to split the allocated buffer (if it is too large)
    // TODO no splitting is present currently. Maybe I'll implement it later.
    // if (allocated_buffer->GetBufferSize() > buffer->GetSize()) {
    //     auto desc = buffer->GetDesc();
    //     desc.size = allocated_buffer->GetBufferSize() - buffer->GetSize();
    //     // Round down to the nearest power of 2 for insertion
    //     desc.size = 1ull << (uint32_t)log2(desc.size);
    //     if (desc.size < RDGBuffer::kMinBufferSize) {
    //
    //     }
    //     auto hash = RDGBuffer::GetResourceClassHash(buffer->GetDesc(), false);
    //     rhi_free_buffer_map_[hash].push_back(allocated_buffer);
    //     allocated_buffer = nullptr;
    // }
    buffer->rhi_buffer_span_ = {allocated_buffer, 0, buffer->GetSize()};
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
    
    texture->rhi_texture_ = allocated_texture;
}



MI_NAMESPACE_END