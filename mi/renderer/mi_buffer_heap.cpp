/*
 * Created: 2025/5/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>

#include <rhi/rhi.h>
#include <rhi/rhi_buffer.h>

#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_buffer_heap.h>

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
     allocation->allocator_ = allocator_;
     return TRef<DeviceUberBufferAllocation>(allocation);
}

TRef<DeviceUberBufferArrayAllocation> DeviceUberBufferArrayInterface::CreateAllocation(uint32_t element_offset, uint32_t element_count) {
    auto allocation = new DeviceUberBufferArrayAllocation();
    allocation->element_offset_ = element_offset;
    allocation->element_count_ = element_count;
    allocation->uber_buffer_array_ = this;
    allocation->allocator_ = allocator_;
    return TRef<DeviceUberBufferArrayAllocation>(allocation);
}

void DeviceUberBufferInterface::SetName(const std::string &name) {
    name_ = name;
}

void DeviceUberBufferArrayInterface::SetName(const std::string &name) {
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

void DeviceUberBufferAllocation::QueueForDestruction() const {
    // If we have an owning allocator, let it retire us a few frames later.
    // Otherwise, fall back to immediate deletion.
    if (allocator_) {
        allocator_->EnqueueForDelayedDestruction(const_cast<DeviceUberBufferAllocation *>(this));
    } else {
        delete this;
    }
}

DeviceUberBufferAllocation::~DeviceUberBufferAllocation() {
    if (uber_buffer_) {
        uber_buffer_->Free(offset_, size_);
    }
}
RHIBufferSpan DeviceUberBufferAllocation::GetRHI() const {
    return uber_buffer_ ? uber_buffer_->GetRHI()->GetSpan(offset_, size_) : RHIBufferSpan{};
}

void DeviceUberBufferArrayAllocation::QueueForDestruction() const {
    if (allocator_) {
        allocator_->EnqueueForDelayedDestruction(const_cast<DeviceUberBufferArrayAllocation *>(this));
    } else {
        delete this;
    }
}

DeviceUberBufferArrayAllocation::~DeviceUberBufferArrayAllocation() {
    if (uber_buffer_array_) {
        uber_buffer_array_->Free(element_offset_, element_count_);
    }
}

RHIBufferSpan DeviceUberBufferArrayAllocation::GetRHI(uint32_t stream_index) const {
    if (!uber_buffer_array_) {
        return {};
    }
    auto * buffer = uber_buffer_array_->GetRHI(stream_index);
    auto element_size = uber_buffer_array_->GetElementSize(stream_index);
    return RHIBufferSpan{
        buffer,
        size_t(element_offset_) * element_size,
        size_t(element_count_) * element_size
    };
}

SimpleDeviceUberBuffer::SimpleDeviceUberBuffer(RHIBufferUsageFlags usage, uint32_t allocation_alignment, size_t initial_size, DeviceBindlessResourceAllocator * allocator):
DeviceUberBufferInterface(usage, allocation_alignment, allocator), segments_(initial_size, allocation_alignment) {
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
            // Update the segment manager
            segments_.ExpandTo(new_uber_buffer->GetBufferSize());
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

size_t SimpleDeviceUberBuffer::GetAllocationLimitByteOffset() const {
    return segments_.GetMaxAllocationEndOffset();
}


void SimpleDeviceUberBuffer::SetName(const std::string &name) {
    DeviceUberBufferInterface::SetName(name);
    if (uber_buffer_) {
        uber_buffer_->SetName(name);
    }
}

SimpleDeviceUberBufferArray::SimpleDeviceUberBufferArray(
    std::vector<DeviceUberBufferArrayDesc> descs,
    uint32_t allocation_alignment_elements,
    uint32_t initial_num_elements,
    DeviceBindlessResourceAllocator * allocator
): DeviceUberBufferArrayInterface(std::move(descs), allocation_alignment_elements, allocator),
   segments_(initial_num_elements, allocation_alignment_elements) {
    mi_check(!descs_.empty(), "DeviceUberBufferArray must have at least one stream.");
    buffers_.reserve(descs_.size());
    for (auto const & desc : descs_) {
        mi_check(desc.element_size > 0, "DeviceUberBufferArray stream element size must be positive.");
        StreamBuffer stream {};
        stream.desc = desc;
        stream.buffer = RHI::Get().CreateBuffer(size_t(initial_num_elements) * desc.element_size, desc.usage);
        if (desc.name && desc.name[0] != '\0') {
            stream.buffer->SetName(desc.name);
        }
        buffers_.push_back(std::move(stream));
    }
}

SimpleDeviceUberBufferArray::~SimpleDeviceUberBufferArray() {
    uint32_t capacity = buffers_.empty() ? 0 : (uint32_t)(buffers_[0].buffer->GetBufferSize() / buffers_[0].desc.element_size);
    if (segments_.GetFreeSegmentCount() != 1 || segments_.GetFreeSegmentSize(0) != capacity) {
        MI_WARN("SimpleDeviceUberBufferArray: Not all segments were freed. Memory leak may occur.");
    }
}

void SimpleDeviceUberBufferArray::ResizeBuffers(uint32_t new_num_elements) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    for (auto & stream : buffers_) {
        size_t old_size = stream.buffer->GetBufferSize();
        auto new_buffer = RHI::Get().CreateBuffer(size_t(new_num_elements) * stream.desc.element_size, stream.desc.usage);
        if (!name_.empty()) {
            std::string stream_name = name_ + " Stream " + std::to_string(&stream - buffers_.data());
            new_buffer->SetName(stream_name);
        } else if (stream.desc.name && stream.desc.name[0] != '\0') {
            new_buffer->SetName(stream.desc.name);
        }
        queue.BufferBarrier(
            stream.buffer->GetSpan(), RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
            RHIGPUAccessFlagBits::kWrite, RHIGPUAccessFlagBits::kTransferRead
        );
        queue.CopyBuffer(
            RHIBufferSpan{stream.buffer.Raw(), 0, old_size},
            RHIBufferSpan{new_buffer.Raw(), 0, old_size}
        );
        queue.BufferBarrier(
            new_buffer->GetSpan(), RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAll,
            RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kAll
        );
        stream.buffer = new_buffer;
    }
    segments_.ExpandTo(new_num_elements);
}

std::pair<uint32_t, bool> SimpleDeviceUberBufferArray::Allocate(uint32_t element_count, bool allow_expansion) {
    size_t offset = segments_.Allocate(element_count);
    if (offset != SIZE_MAX) {
        mi_check(offset <= UINT32_MAX, "SimpleDeviceUberBufferArray allocation offset exceeds uint32 range.");
        return {(uint32_t)offset, true};
    }
    if (!allow_expansion) {
        return {UINT32_MAX, false};
    }

    uint32_t current_num_elements = buffers_.empty() ? 0 : (uint32_t)(buffers_[0].buffer->GetBufferSize() / buffers_[0].desc.element_size);
    uint32_t new_num_elements = std::max(
        uint32_t(std::max<size_t>(size_t(current_num_elements * 3) / 2, size_t(current_num_elements) + element_count)),
        current_num_elements + element_count
    );
    ResizeBuffers(new_num_elements);

    offset = segments_.Allocate(element_count);
    mi_assert(offset != SIZE_MAX, "SimpleDeviceUberBufferArray: Allocation should succeed after expansion.");
    mi_check(offset <= UINT32_MAX, "SimpleDeviceUberBufferArray allocation offset exceeds uint32 range after expansion.");
    return {(uint32_t)offset, true};
}

void SimpleDeviceUberBufferArray::Free(uint32_t element_offset, uint32_t element_count) {
    segments_.Free(element_offset, element_count);
}

RHIBuffer * SimpleDeviceUberBufferArray::GetRHI(uint32_t stream_index) const {
    mi_check(stream_index < buffers_.size(), "Stream index out of range.");
    return buffers_[stream_index].buffer.Raw();
}

uint32_t SimpleDeviceUberBufferArray::GetAllocationLimitElementOffset() const {
    return (uint32_t)segments_.GetMaxAllocationEndOffset();
}

void SimpleDeviceUberBufferArray::SetName(const std::string &name) {
    DeviceUberBufferArrayInterface::SetName(name);
    for (size_t i = 0; i < buffers_.size(); ++i) {
        buffers_[i].buffer->SetName(name + " Stream " + std::to_string(i));
    }
}

MI_NAMESPACE_END
