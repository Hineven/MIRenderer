/*
 * Created: 2025/3/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rhi/rhi.h"
#include "rhi/rhi_buffer.h"
#include "rdg/rdg_pool.h"
#include <rdg/rdg_resource.h>
MI_NAMESPACE_BEGIN

RDGResourcePool::RDGResourcePool() {

}

RDGResourcePool::~RDGResourcePool() {
    printf("pool destruction\n");
}

TRef<RDGResourcePool> RDGResourcePool::Create() {
    return TRef<RDGResourcePool>(new RDGResourcePool());
}

void RDGResourcePool::AllocateResource(RDGBuffer *buffer) {
    // TODO better strategy. Now I'll only implement a simple one
    assert(!buffer->IsAllocated() && "This buffer should not be allocated already.");
    assert(buffer->desc_.size > 0 && "Buffer size should be greater than 0.");
    auto & RHI = RHI::Get();
    RDGPoolFreeBufferRecord allocated = {};
    size_t requested_size = buffer->GetSize();
    if (buffer->dedicated_) {
        // Allocate a new buffer
        auto desc = buffer->GetDesc();
        auto rhi_buffer = RHI.CreateBuffer(desc);
        // Round up the size for alignment requirement
        desc.size = RoundUp(desc.size, RDGBuffer::kMinBufferSize);
        allocated = {rhi_buffer.Raw(), RHIGPUAccessFlagBits::kNone};
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
                    // Too large to reuse, stop searching and allocate a new buffer block
                    break;
                }
                auto hash = RDGBuffer::GetResourceClassHash(desc, false);
                auto & slot = rhi_free_buffer_map_[hash];
                if (!slot.empty()) {
                    // Found one, allocate it
                    allocated = slot.back();
                    slot.pop_back();
                    break;
                }
            }
            // Pool memory ran out, allocate a new buffer block
            if (!allocated.buffer) {
                desc.size = kBufferBlockSize;
                auto rhi_buffer = RHI.CreateBuffer(desc);
                allocated = {rhi_buffer.Raw(), RHIGPUAccessFlagBits::kNone};
                rhi_buffer_references_.emplace_back(std::move(rhi_buffer));
            }
        } else {
            // Use dedicated allocation for buffers larger than buffer block
            auto desc = buffer->GetDesc();
            // Round up the size for alignment requirement
            desc.size = RoundUp(desc.size, RDGBuffer::kMinBufferSize);
            auto rhi_buffer = RHI.CreateBuffer(desc);
            allocated = {rhi_buffer.Raw(), RHIGPUAccessFlagBits::kNone};
            rhi_buffer_references_.emplace_back(std::move(rhi_buffer));
        }
    }
    buffer->rhi_buffer_span_ = {allocated.buffer, 0, buffer->GetSize()};
    buffer->usage_ = allocated.last_usage;
}

void RDGResourcePool::RecycleResource(RDGBuffer *buffer) {
    auto hash = buffer->GetResourceClassHash();
    rhi_free_buffer_map_[hash].push_back({
        buffer->rhi_buffer_span_.buffer,
        buffer->usage_
    });
    buffer->rhi_buffer_span_ = {};
    buffer->usage_ = {};
}

void RDGResourcePool::AllocateResource(RDGTexture *texture) {
    assert(!texture->IsAllocated() && "This texture should not be allocated already.");
    auto & RHI = RHI::Get();
    auto hash = texture->GetResourceClassHash();
    auto & slot = rhi_free_texture_map_[hash];
    RDGPoolFreeTextureRecord allocated = {};
    if (slot.empty()) {
        // Allocate a new texture
        auto desc = texture->GetDesc();
        auto rhi_texture = RHI.CreateTexture(desc);
        allocated = {rhi_texture.Raw(), RDGTextureUsageType::kNone};
        rhi_texture_references_.emplace_back(std::move(rhi_texture));
    }
    else {
        allocated = slot.back();
        slot.pop_back();
    }
    
    texture->rhi_texture_ = allocated.texture;
    texture->usage_ = allocated.last_usage;
}

void RDGResourcePool::RecycleResource(RDGTexture *texture) {
    assert(texture->IsAllocated() && "This texture should be allocated.");
    auto hash = texture->GetResourceClassHash();
    rhi_free_texture_map_[hash].emplace_back(texture->rhi_texture_, texture->usage_);
    texture->rhi_texture_ = nullptr;
    texture->usage_ = {};
}



MI_NAMESPACE_END