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
    if (num_active_buffers_ + num_active_textures_ != 0)
        MI_LOG(MIInfraLogType::kError, "RDGResourcePool is destroyed with {} / {} active allocations left!", num_active_buffers_, num_active_textures_);
}

TRef<RDGResourcePool> RDGResourcePool::Create() {
    return {new RDGResourcePool()};
}

RDGResourcePool::RDGPoolFreeBufferRecord RDGResourcePool::AllocateBufferBlock (RHIBufferDesc for_buffer_desc) {
    // Allocate a new buffer
    auto rhi_buffer = RHI::Get().CreateBuffer(for_buffer_desc);
#ifndef NDEBUG
    rhi_buffer->SetName("Unnamed RDG pool buffer #" + std::to_string(rhi_buffer_references_.size()));
#endif
    RDGPoolFreeBufferRecord allocated = {
        rhi_buffer.Raw(), for_buffer_desc.size, {}, {}, {}
    };
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
    if (!buffer->name_.empty()) {
        allocated.buffer->SetName(buffer->name_);
    }
    buffer->rhi_buffer_span_ = {allocated.buffer, 0, requested_size};
    buffer->read_stages_ = allocated.last_read_stages;
    buffer->write_stages_ = allocated.last_write_stages;
    buffer->read_access_ = allocated.last_access & RHIGPUAccessFlagBits::kRead;
    buffer->write_access_ = allocated.last_access & RHIGPUAccessFlagBits::kWrite;

    num_active_buffers_ ++;
}

void RDGResourcePool::RecycleResource(RDGBuffer *buffer) {
    // printf("RecycleBuffer %s size %llu\n", buffer->GetName().c_str(), buffer->GetAllocationSize());
    auto hash = buffer->GetResourceClassHash();
    rhi_free_buffer_map_[hash].emplace_back(
        buffer->rhi_buffer_span_.buffer,
        buffer->desc_.size,
        buffer->read_stages_,
        buffer->write_stages_,
        buffer->read_access_ | buffer->write_access_
    );
    buffer->rhi_buffer_span_ = {};
    buffer->read_access_ = {};
    buffer->write_access_ = {};
    buffer->read_stages_ = {};
    buffer->write_stages_ = {};
    buffer->desc_.size = 0;

    num_active_buffers_ --;
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
        allocated = {rhi_texture.Raw(), {}, {}, {}};
        total_device_memory_usage_ += rhi_texture->GetSize();

        rhi_texture_references_.emplace_back(std::move(rhi_texture));
    }
    else {
        allocated = slot.back();
        slot.pop_back();
    }
    
    texture->rhi_texture_ = allocated.texture;
    texture->read_access_ = allocated.last_access & RHIGPUAccessFlagBits::kRead;
    texture->write_access_ = allocated.last_access & RHIGPUAccessFlagBits::kWrite;
    texture->read_stages_ = allocated.last_read_stages;
    texture->write_stages_ = allocated.last_write_stages;
    texture->current_layout_ = RHITextureLayoutType::kUndefined;

    num_active_textures_ ++;
}

void RDGResourcePool::RecycleResource(RDGTexture *texture) {
    assert(texture->IsAllocated() && "This texture should be allocated.");
    auto hash = texture->GetResourceClassHash();
    rhi_free_texture_map_[hash].emplace_back(texture->rhi_texture_,
        texture->read_stages_, texture->write_stages_,
        texture->read_access_ | texture->write_access_);
    texture->rhi_texture_ = nullptr;
    texture->read_access_ = {};
    texture->write_access_ = {};
    texture->read_stages_ = {};
    texture->write_stages_ = {};
    texture->current_layout_ = RHITextureLayoutType::kUndefined;

    num_active_textures_ --;
}



MI_NAMESPACE_END