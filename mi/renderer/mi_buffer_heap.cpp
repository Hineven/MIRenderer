/*
 * Created: 2025/5/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "renderer/mi_buffer_heap.h"

#include "rhi/rhi.h"
#include "rhi/rhi_buffer.h"

MI_NAMESPACE_BEGIN
DeviceBufferHeapBuffer::~DeviceBufferHeapBuffer() {
    heap->Free(buffer);
}

TRef<DeviceBufferHeapBuffer> DeviceBufferHeapInterface::AllocateRefCounted(uint32_t size) {
    auto buf = Allocate(size);
    auto ref = TRef<DeviceBufferHeapBuffer>(new DeviceBufferHeapBuffer);
    ref->buffer = buf;
    ref->heap = this;
    return std::move(ref);
}


SimpleDeviceBufferHeap::BufferBlock::BufferBlock() {

}

SimpleDeviceBufferHeap::BufferBlock::~BufferBlock() {

}

SimpleDeviceBufferHeap::SimpleDeviceBufferHeap (RHIBufferUsageFlags usage, uint32_t alignment, uint32_t buffer_block_size) :
DeviceBufferHeapInterface(usage, alignment) {
    default_buffer_block_size_ = buffer_block_size;
    assert(default_buffer_block_size_ % alignment == 0);
}

SimpleDeviceBufferHeap::~SimpleDeviceBufferHeap() {
    for (auto& buffer_block : buffer_blocks_) {
        if (buffer_block.buffer) {
            mi_assert_nothrow(buffer_block.free_segments_.size() == 1 && buffer_block.free_segments_.begin()->size == default_buffer_block_size_,
                "GPUHeapBuffers not fully freed.");
        }
    }
}

int SimpleDeviceBufferHeap::FindBufferBlockIndex(RHIBuffer *buffer) const {
    for (int i = 0; i < buffer_blocks_.size(); i++) {
        if (buffer == buffer_blocks_[i].buffer) {
            return i;
        }
    }
    return -1;
}

void SimpleDeviceBufferHeap::AddNewBlock(size_t block_size, size_t first_allocation_size) {
    auto new_buffer = RHI::Get().CreateBuffer(block_size, usage_);
    BufferBlock new_block;
    new_block.buffer = new_buffer;
    if (block_size != first_allocation_size) {
        new_block.free_segments_.emplace(first_allocation_size, block_size - first_allocation_size);
    }
    buffer_blocks_.push_back(new_block);
}


RHIBufferSpan SimpleDeviceBufferHeap::Allocate(uint32_t size) {
    auto guard = std::lock_guard(mutex_);

    uint32_t aligned_size = (size + allocation_alignment - 1) & ~(allocation_alignment - 1);

    for (auto& buffer_block : buffer_blocks_) {
        for (auto it = buffer_block.free_segments_.begin(); it != buffer_block.free_segments_.end(); ++it) {
            if (it->size >= aligned_size) {
                uint32_t start_offset = it->start_offset;
                size_t remaining_size = it->size - aligned_size;

                buffer_block.free_segments_.erase(it);

                if (remaining_size > 0) {
                    buffer_block.free_segments_.emplace(start_offset + aligned_size, remaining_size);
                }

                return RHIBufferSpan{buffer_block.buffer.Raw(), start_offset, aligned_size};
            }
        }
    }

    // Add a new buffer block
    if (max_num_buffer_blocks_ > 0 && buffer_blocks_.size() >= max_num_buffer_blocks_) {
        MI_WARN("SimpleDeviceBufferHeap: Maximum number of buffer blocks reached. Cannot allocate more.");
        return RHIBufferSpan{};
    }

    uint32_t new_block_size = std::max(default_buffer_block_size_, aligned_size);
    AddNewBlock(new_block_size, aligned_size);

    return RHIBufferSpan{buffer_blocks_.back().buffer.Raw(), 0, aligned_size};
}

void SimpleDeviceBufferHeap::Free (RHIBufferSpan allocation) {
    auto guard = std::lock_guard(mutex_);

    auto span = allocation;
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
}

RHIBuffer * SimpleDeviceBufferHeap::GetHeapBufferBlock (uint32_t block_index) const {
    mi_assert(block_index < buffer_blocks_.size(), "Buffer block index out of range.");
    return buffer_blocks_[block_index].buffer.Raw();
}

uint32_t SimpleDeviceBufferHeap::GetNumHeapBufferBlocks () const {
    return (uint32_t)buffer_blocks_.size();
}

void SimpleDeviceBufferHeap::SetNumBufferBlockLimit(uint32_t num) {
    mi_check(buffer_blocks_.size() <= num, "Buffer block limit is less than the current number of buffer blocks.");
    max_num_buffer_blocks_ = num;
}

void SimpleDeviceBufferHeap::PreAllocateBlocks(uint32_t num_blocks) {
    for (int i = 0; i < num_blocks; i++) AddNewBlock(default_buffer_block_size_, 0);
}


MI_NAMESPACE_END