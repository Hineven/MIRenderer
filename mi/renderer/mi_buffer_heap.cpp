/*
 * Created: 2025/5/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>
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

TRef<DeviceUberBufferAllocation> DeviceUberBufferInterface::CreateAllocation(size_t offset, size_t size) {
    auto allocation = new DeviceUberBufferAllocation();
    allocation->offset_ = offset;
    allocation->size_ = size;
    allocation->uber_buffer_ = this;
    return TRef<DeviceUberBufferAllocation>(allocation);
}

void DeviceUberBufferInterface::SetName(const std::string &name) {
    name_ = name;
}


void DeviceBufferHeapInterface::SetName(const std::string &name) {
    name_ = name;
}

SimpleDeviceBufferHeap::BufferBlock::BufferBlock(
    size_t default_buffer_block_size_, size_t allocation_alignment
): segments_(default_buffer_block_size_, allocation_alignment) {
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
            mi_assert_nothrow(buffer_block.segments_.GetFreeSegmentCount() == 1
                && buffer_block.segments_.GetFreeSegmentSize(0) == default_buffer_block_size_,
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

void SimpleDeviceBufferHeap::AddNewBlock(size_t block_size) {
    auto new_buffer = RHI::Get().CreateBuffer(block_size, usage_);
    if (!name_.empty()) new_buffer->SetName(name_);
    BufferBlock new_block(default_buffer_block_size_, allocation_alignment_);
    new_block.buffer = new_buffer;
    buffer_blocks_.push_back(new_block);
}


RHIBufferSpan SimpleDeviceBufferHeap::Allocate(uint32_t size) {
    auto guard = std::lock_guard(mutex_);

    uint32_t aligned_size = (size + allocation_alignment_ - 1) & ~(allocation_alignment_ - 1);

    for (auto& buffer_block : buffer_blocks_) {
        auto offset = buffer_block.segments_.Allocate(aligned_size);
        if (offset != SIZE_MAX)
            return RHIBufferSpan{buffer_block.buffer.Raw(), offset, aligned_size};
    }

    // Add a new buffer block
    if (max_num_buffer_blocks_ > 0 && buffer_blocks_.size() >= max_num_buffer_blocks_) {
        MI_WARN("SimpleDeviceBufferHeap: Maximum number of buffer blocks reached. Cannot allocate more.");
        return RHIBufferSpan{};
    }

    uint32_t new_block_size = std::max(default_buffer_block_size_, aligned_size);
    AddNewBlock(new_block_size);

    return RHIBufferSpan{buffer_blocks_.back().buffer.Raw(), 0, aligned_size};
}

void SimpleDeviceBufferHeap::Free (RHIBufferSpan allocation) {
    auto guard = std::lock_guard(mutex_);

    auto span = allocation;
    auto buffer_block_index = FindBufferBlockIndex(span.buffer);
    assert(buffer_block_index != -1);
    auto & buffer_block = buffer_blocks_[buffer_block_index];
    buffer_block.segments_.Free(span.offset, span.size);
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

uint32_t SimpleDeviceBufferHeap::GetBufferBlockIndex(RHIBuffer *buffer) const {
    for (int i = 0; i < (int)buffer_blocks_.size(); i++) {
        if (buffer == buffer_blocks_[i].buffer) {
            return (uint32_t)i;
        }
    }
    return UINT32_MAX; // Not found
}


void SimpleDeviceBufferHeap::SetName(const std::string &name) {
    DeviceBufferHeapInterface::SetName(name);
    for (auto [i, e] : std::views::enumerate(buffer_blocks_)) {
        if (e.buffer) {
            e.buffer->SetName(name + " Block " + std::to_string(i));
        }
    }
}

void SimpleDeviceBufferHeap::PreAllocateBlocks(uint32_t num_blocks) {
    for (int i = 0; i < (int)num_blocks; i++) AddNewBlock(default_buffer_block_size_);
}

DeviceUberBufferAllocation::~DeviceUberBufferAllocation() {
    if (uber_buffer_) {
        uber_buffer_->Free(offset_, size_);
    }
}
RHIBufferSpan DeviceUberBufferAllocation::GetRHI() const {
    return uber_buffer_ ? uber_buffer_->GetRHI()->GetSpan(offset_, size_) : RHIBufferSpan{};
}

SimpleDeviceUberBuffer::SimpleDeviceUberBuffer(RHIBufferUsageFlags usage, uint32_t allocation_alignment, size_t initial_size):
DeviceUberBufferInterface(usage, allocation_alignment), segments_(initial_size, allocation_alignment) {
    uber_buffer_ = RHI::Get().CreateBuffer(initial_size, usage);
}

SimpleDeviceUberBuffer::~SimpleDeviceUberBuffer() {
    if (segments_.GetFreeSegmentCount() != 1 || segments_.GetFreeSegmentSize(0) != uber_buffer_->GetBufferSize()) {
        MI_WARN("SimpleDeviceUberBuffer: Not all segments were freed. Memory leak may occur.");
    }
}

std::pair<size_t, bool> SimpleDeviceUberBuffer::Allocate(uint32_t size, bool allow_expansion) {
    auto offset = segments_.Allocate(size);
    if (offset == SIZE_MAX) {
        if (allow_expansion) {
            // Expand and duplicate the buffer
            auto new_uber_buffer = RHI::Get().CreateBuffer(
                std::max((size_t)(uber_buffer_->GetBufferSize() * 1.5), uber_buffer_->GetBufferSize() + size),
                usage_
            );
            if (!new_uber_buffer) {
                MI_WARN("SimpleDeviceUberBuffer: Failed to expand the buffer. Allocation failed.");
                return {TRef<DeviceUberBufferAllocation>(), false};
            }
            auto & queue = RHI::Get().GetGraphicsCommandQueue();
            // Add barriers for transfer read
            queue.BufferBarrier(
                uber_buffer_->GetSpan(), RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
                RHIGPUAccessFlagBits::kWrite, RHIGPUAccessFlagBits::kTransferRead
            );
            // Copy the old data to the new buffer
            queue.CopyBuffer(
                RHIBufferSpan{uber_buffer_.Raw(), 0, uber_buffer_->GetBufferSize()},
                RHIBufferSpan{new_uber_buffer.Raw(), 0, uber_buffer_->GetBufferSize()}
            );
            // Add barriers for transfer write
            queue.BufferBarrier(
                new_uber_buffer->GetSpan(), RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAll,
                RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kAll
            );
            // Replace the old buffer with the new one
            uber_buffer_ = new_uber_buffer;
            // Allocate the segment again
            offset = segments_.Allocate(size);
            if (offset != SIZE_MAX) [[likely]] {
                // Allocation succeeded
                return {offset, true};
            } else {
                // Allocation failed again, which is essentially impossible since we just expanded the buffer.
                mi_assert(false, "SimpleDeviceUberBuffer: This should not be reachable.");
                return {SIZE_MAX, false};
            }
        } else {
            // Allocation failed
            return {SIZE_MAX, false};
        }
    } else {
        // Allocation succeeded
        return {offset, true};
    }
}

void SimpleDeviceUberBuffer::Free(size_t offset, size_t size) {
    segments_.Free(offset, size);
}

RHIBuffer *SimpleDeviceUberBuffer::GetRHI() const {
    return uber_buffer_.Raw();
}

void SimpleDeviceUberBuffer::SetName(const std::string &name) {
    DeviceUberBufferInterface::SetName(name);
    if (uber_buffer_) {
        uber_buffer_->SetName(name);
    }
}

MI_NAMESPACE_END