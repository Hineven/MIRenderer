/*
 * Created: 2025/3/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rdg/rdg_pool.h"

#include "rhi/rhi.h"
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
    if (buffer->desc_.usage & RHIBufferUsageFlagBits::kUniform) {
        // Uniform buffer are allocated in a separate pool
        AllocateUniformBuffer(buffer);
        return ;
    }
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
    if (buffer->desc_.usage & RHIBufferUsageFlagBits::kUniform) {
        // Uniform buffers will not be recycled.
        return;
    }
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

void RDGResourcePool::RecycleResource(RDGTexture *texture) {
    assert(texture->IsAllocated() && "This texture should be allocated.");
    auto hash = texture->GetResourceClassHash();
    rhi_free_texture_map_[hash].push_back(texture->rhi_texture_);
    texture->rhi_texture_ = nullptr;
}


void RDGResourcePool::AllocateUniformBuffer(RDGBuffer *buffer) {
    assert(buffer->desc_.usage == RHIBufferUsageFlagBits::kUniform && "This buffer should be (and only be) a uniform buffer.");
    assert(buffer->desc_.size > 0 && "Uniform buffer size should be greater than 0.");
    assert(!buffer->dedicated_ && "Uniform buffers should not be dedicated");
    if (!buffer->IsAllocated()) {
        // Simply allocate the buffer within the uniform buffer pool
        auto size = RoundUp(buffer->desc_.size, C::kUniformBufferAlignment);
        assert((uint32_t)size <= C::kRDGPoolUniformBufferBlockSize && "Uniform buffer size exceeds the maximum size."); // TODO implement a better strategy
        if (rhi_uniform_buffer_index_ >= rhi_uniform_buffer_references_.size()
            || rhi_uniform_buffer_offset_ + size > C::kRDGPoolUniformBufferBlockSize) {
            if (rhi_uniform_buffer_index_ + 1 >= rhi_uniform_buffer_references_.size()) {
                // Running out, allocate a new block
                auto desc = RHIBufferDesc{.size = C::kRDGPoolUniformBufferBlockSize, .usage = RHIBufferUsageFlagBits::kUniform};
                auto rhi_buffer = RHI::Get().CreateBuffer(desc);
                rhi_uniform_buffer_references_.emplace_back(std::move(rhi_buffer));
                // Also remember to duplicate a new block in the staging buffer pool
                desc.usage = RHIBufferUsageFlagBits::kTransferSrc | RHIBufferUsageFlagBits::kStaging;
                auto staging_buffer = RHI::Get().CreateBuffer(desc);
                staging_buffer->Map();
                rhi_staging_buffer_references_.emplace_back(std::move(staging_buffer));
            }
            // Move to the next block
            rhi_uniform_buffer_index_ = std::min((int)rhi_uniform_buffer_index_ + 1, (int)rhi_uniform_buffer_references_.size() - 1);
            rhi_uniform_buffer_offset_ = 0;
        }
        buffer->rhi_buffer_span_ = {rhi_uniform_buffer_references_[rhi_uniform_buffer_index_].Raw(), rhi_uniform_buffer_offset_, size};
        // Assign the staging buffer for the uniform buffer
        buffer->staging_mapped_ptr_ = (std::byte*)rhi_staging_buffer_references_[rhi_uniform_buffer_index_]->Map() + rhi_uniform_buffer_offset_;
        rhi_uniform_buffer_offset_ += (uint32_t)size;
    }
}

void RDGResourcePool::StageUniformBuffers(RHICommandQueueGraphics &queue) {
    // TODO support multiple stage operations (more than 1 graphs share the same resource pool)
    assert(!buffers_staged_ && "Twice staging uniform buffers is not allowed.");
    for (int i = 0; i < ((int)rhi_uniform_buffer_index_ - 1); i++) {
        queue.CopyBuffer(
            rhi_staging_buffer_references_[i]->GetSpan(),
            rhi_uniform_buffer_references_[i]->GetSpan()
        );
    }
    if (rhi_uniform_buffer_index_ > 0) {
        queue.CopyBuffer(
            {rhi_staging_buffer_references_[rhi_uniform_buffer_index_ - 1].Raw(), 0, rhi_uniform_buffer_offset_},
            {rhi_uniform_buffer_references_[rhi_uniform_buffer_index_ - 1].Raw(), 0, rhi_uniform_buffer_offset_}
        );
    }
    buffers_staged_ = true;
}

MI_NAMESPACE_END