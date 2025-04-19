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
    // Allocate a new buffer
    auto rhi_buffer = RHI::Get().CreateBuffer(for_buffer_desc);
    RDGPoolFreeBufferRecord allocated = {rhi_buffer.Raw(), for_buffer_desc.size, RHIGPUAccessFlagBits::kNone};
    rhi_buffer_references_.emplace_back(std::move(rhi_buffer));

    total_device_memory_usage_ += for_buffer_desc.size;

    return allocated;
}

void RDGResourcePool::AllocateResource(RDGBuffer *buffer) {
    // printf("AllocateResource size %llu\n", buffer->GetRequestedSize());
    assert(!buffer->IsImported() && "Imported buffer should not be allocated by the pool.");
    // TODO better strategy. Now I'll only implement a simple one
    assert(!buffer->IsAllocated() && "This buffer should not be allocated already.");
    assert(buffer->requested_size_ > 0 && "Buffer size should be greater than 0.");
    RDGPoolFreeBufferRecord allocated = {};
    size_t requested_size = buffer->GetRequestedSize();
    if (buffer->dedicated_) {
        // Allocate a new buffer
        allocated = AllocateBufferBlock(buffer->GetDesc());
    } else {
        auto & desc = buffer->desc_;
        desc.size = RDGBuffer::GetBestAllocationSizeFromRequestedSize(requested_size);
        if (desc.size <= kBufferBlockSize) {
            while (true) {
                auto hash = RDGBuffer::GetResourceClassHash(desc, false);
                auto & slot = rhi_free_buffer_map_[hash];
                if (!slot.empty()) {
                    // Found one, allocate it
                    allocated = slot.back();
                    slot.pop_back();
                    break;
                }
                if (log2(desc.size / requested_size) >= kBufferReusingThresholdLog2
                    || desc.size > kBufferReusingAbsoluteThreshold) {
                    // Too large to reuse, stop searching and allocate a new buffer block
                    break;
                }
                desc.size *= 2;
            }
            // Pool memory ran out, allocate a new buffer block
            if (!allocated.buffer)
                allocated = AllocateBufferBlock(desc);
        } else {
            // use dedicated allocation for large buffers
            allocated = AllocateBufferBlock(desc);
        }
    }
    buffer->rhi_buffer_span_ = {allocated.buffer, 0, requested_size};
    buffer->usage_ = allocated.last_usage;
}

void RDGResourcePool::RecycleResource(RDGBuffer *buffer) {
    // printf("RecycleBuffer size %llu\n", buffer->GetAllocationSize());
    auto hash = buffer->GetResourceClassHash();
    rhi_free_buffer_map_[hash].emplace_back(
        buffer->rhi_buffer_span_.buffer,
        buffer->desc_.size,
        buffer->usage_
    );
    buffer->rhi_buffer_span_ = {};
    buffer->usage_ = {};
    buffer->desc_.size = 0;
}

void RDGResourcePool::AllocateResource(RDGTexture *texture) {
    assert(!texture->IsImported() && "Imported texture should not be allocated by the pool.");
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