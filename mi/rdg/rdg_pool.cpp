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
}

TRef<RDGResourcePool> RDGResourcePool::Create() {
    return TRef<RDGResourcePool>(new RDGResourcePool());
}

RDGResourcePool::RDGPoolFreeBufferRecord RDGResourcePool::AllocateBufferBlock (RHIBufferDesc for_buffer_desc) {
    // Round up the size for alignment requirement
    for_buffer_desc.size = RoundUp(for_buffer_desc.size, RDGBuffer::kMinBufferSize);
    // Allocate a new buffer
    auto rhi_buffer = RHI::Get().CreateBuffer(for_buffer_desc);
    RDGPoolFreeBufferRecord allocated = {rhi_buffer.Raw(), RHIGPUAccessFlagBits::kNone};
    rhi_buffer_references_.emplace_back(std::move(rhi_buffer));

    total_device_memory_usage_ += for_buffer_desc.size;

    return allocated;
}

void RDGResourcePool::AllocateResource(RDGBuffer *buffer) {
    assert(!buffer->is_imported_ && "Imported buffer should not be allocated by the pool.");
    // TODO better strategy. Now I'll only implement a simple one
    assert(!buffer->IsAllocated() && "This buffer should not be allocated already.");
    assert(buffer->desc_.size > 0 && "Buffer size should be greater than 0.");
    RDGPoolFreeBufferRecord allocated = {};
    size_t requested_size = buffer->GetSize();
    if (buffer->dedicated_) {
        // Allocate a new buffer
        allocated = AllocateBufferBlock(buffer->GetDesc());
    } else {
        if (buffer->desc_.size <= kBufferBlockSize) {
            // Running out, find a larger one.
            auto desc = buffer->GetDesc();
            size_t min_pow2_size = UINT64_MAX;
            for (int i = RDGBuffer::kMinBufferSizeLog2;
                i <= kBufferBlockSizeLog2; i++) {
                size_t block_size = 1ull << i;
                if (block_size < requested_size) continue;
                min_pow2_size = std::min(min_pow2_size, block_size);
                desc.size = block_size;
                int ratio = i - (int)log2(requested_size);
                if (i >= kBufferReusingAbsoluteThresholdLog2
                    || (i != RDGBuffer::kMinBufferSizeLog2 && ratio > kBufferReusingThresholdLog2)) {
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
                desc.size = min_pow2_size;
                allocated = AllocateBufferBlock(desc);
            }
        } else {
            // Use dedicated allocation for buffers larger than buffer block, no longer rounding it up to the next power of 2
            auto desc = buffer->GetDesc();
            desc.size = RoundUp(desc.size, RDGBuffer::kMinBufferSize);
            allocated = AllocateBufferBlock(desc);
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
    assert(!texture->is_imported_ && "Imported texture should not be allocated by the pool.");
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
        total_device_memory_usage_ += rhi_texture->GetSize();

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