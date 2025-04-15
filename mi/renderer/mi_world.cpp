/*
 * Created: 2025/4/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_world.h"

#include <rhi/rhi.h>

MI_NAMESPACE_BEGIN
    GPUBufferHeapBuffer::~GPUBufferHeapBuffer() {
    heap->Free(this);
}

void GPUBufferHeapInterface::Free (GPUBufferHeapBuffer * buffer) {
    buffer->buffer = {};
    buffer->heap = nullptr;
}

TRef<GPUBufferHeapBuffer> GPUBufferHeapInterface::CreateBuffer(RHIBufferSpan span) {
    auto buf = TRef<GPUBufferHeapBuffer>(new GPUBufferHeapBuffer());
    buf->buffer = span;
    buf->heap = this;
    return std::move(buf);
}

SimpleGPUBufferHeap::SimpleGPUBufferHeap (RHIBufferUsageFlags usage, uint32_t alignment, uint32_t buffer_block_size) :
GPUBufferHeapInterface(usage, alignment) {
    buffer_block_size_ = buffer_block_size;
    assert(buffer_block_size_ % alignment == 0);
}

SimpleGPUBufferHeap::~SimpleGPUBufferHeap() {
    for (auto& buffer_block : buffer_blocks_) {
        if (buffer_block.buffer) {
            mi_assert(buffer_block.free_segments_.size() == 1 && buffer_block.free_segments_.begin()->size == buffer_block_size_,
                "GPUHeapBuffers not fully freed.");
        }
    }
}

int SimpleGPUBufferHeap::FindBufferBlockIndex(RHIBuffer *buffer) const {
    for (int i = 0; i < buffer_blocks_.size(); i++) {
        if (buffer == buffer_blocks_[i].buffer) {
            return i;
        }
    }
    return -1;
}


TRef<GPUBufferHeapBuffer> SimpleGPUBufferHeap::Allocate(uint32_t size) {
    auto guard = std::lock_guard(mutex_);

    uint32_t aligned_size = (size + allocation_alignment - 1) & ~(allocation_alignment - 1);

    for (auto& buffer_block : buffer_blocks_) {
        for (auto it = buffer_block.free_segments_.begin(); it != buffer_block.free_segments_.end(); ++it) {
            if (it->size >= aligned_size) {
                uint32_t start_offset = it->start_offset;
                uint32_t remaining_size = it->size - aligned_size;

                buffer_block.free_segments_.erase(it);

                if (remaining_size > 0) {
                    buffer_block.free_segments_.emplace(start_offset + aligned_size, remaining_size);
                }

                auto result = CreateBuffer(RHIBufferSpan{buffer_block.buffer.Raw(), start_offset, aligned_size});
                return std::move(result);
            }
        }
    }

    uint32_t new_buffer_size = std::max(buffer_block_size_, aligned_size);

    auto new_buffer = RHI::Get().CreateBuffer(new_buffer_size, usage_);
    BufferBlock new_block;
    new_block.buffer = new_buffer;

    if (new_buffer_size > aligned_size) {
        new_block.free_segments_.emplace(aligned_size, new_buffer_size - aligned_size);
    }

    buffer_blocks_.push_back(new_block);

    auto result = CreateBuffer(RHIBufferSpan{new_buffer.Raw(), 0, aligned_size});
    return std::move(result);
}

void SimpleGPUBufferHeap::Free (GPUBufferHeapBuffer * buffer) {
    assert(buffer->GetHeap() == this);
    auto guard = std::lock_guard(mutex_);

    auto span = buffer->GetRHI();
    auto buffer_block_index = FindBufferBlockIndex(span.buffer);
    assert(buffer_block_index != -1);
    auto & buffer_block = buffer_blocks_[buffer_block_index];
    auto result = buffer_block.free_segments_.emplace(
        span.offset, span.size
    );
    assert(result.second);
    auto it = result.first;
    // Merge with the previous segment if possible
    if (it != buffer_block.free_segments_.begin()) {
        auto prev_it = std::prev(it);
        if (prev_it->start_offset + prev_it->size == it->start_offset) {
            prev_it->size += it->size;
            buffer_block.free_segments_.erase(it);
            it = prev_it;
        }
    }
    // Merge with the next segment if possible
    if (std::next(it) != buffer_block.free_segments_.end()) {
        auto next_it = std::next(it);
        if (it->start_offset + it->size == next_it->start_offset) {
            it->size += next_it->size;
            buffer_block.free_segments_.erase(next_it);
        }
    }
    GPUBufferHeapInterface::Free(buffer);
}

MI_NAMESPACE_END