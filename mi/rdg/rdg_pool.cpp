/*
 * Created: 2025/3/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rhi/rhi.h"
#include "rhi/rhi_buffer.h"
#include "rdg/rdg_pool.h"
#include <rdg/rdg_resource.h>
#include "core/infra.h"
#include <algorithm>
MI_NAMESPACE_BEGIN


RDGResourcePool::RDGResourcePool() {

}

RDGResourcePool::~RDGResourcePool() {
    if (num_active_buffers_ + num_active_textures_ != 0)
        MI_LOG(MIInfraLogType::kError, "RDGResourcePool is destroyed with {} / {} active allocations left!", num_active_buffers_, num_active_textures_);
    for (auto & [hash, allocs] : free_buffer_allocations_) {
        for (auto * alloc : allocs) delete alloc;
    }
    for (auto & [hash, allocs] : free_texture_allocations_) {
        for (auto * alloc : allocs) delete alloc;
    }
}

TRef<RDGResourcePool> RDGResourcePool::Create() {
    return {new RDGResourcePool()};
}

// --- Buffer allocation ---

RDGPoolBufferAllocation * RDGResourcePool::AllocateNewBufferBlock(RHIBufferDesc desc) {
    auto rhi_buffer = RHI::Get().CreateBuffer(desc);
#if MI_ENABLE_RHI_OBJECT_NAMING
    rhi_buffer->SetName("Unnamed RDG pool buffer #" + std::to_string(rhi_buffer_references_.size()));
#endif
    auto * alloc = new RDGPoolBufferAllocation();
    alloc->buffer = rhi_buffer.Raw();
    alloc->allocation_size = desc.size;
    rhi_buffer_references_.emplace_back(std::move(rhi_buffer));
    total_device_memory_usage_ += desc.size;
    return alloc;
}

RDGPoolBufferAllocation * RDGResourcePool::FindOrCreateBufferAllocation(RDGBuffer *buffer) {
    size_t requested_size = buffer->GetRequestedSize();
    if (buffer->dedicated_) {
        // Allocate a new buffer for dedicated resources
        auto * alloc = AllocateNewBufferBlock(buffer->GetDesc());
        alloc->resource_class_hash = buffer->GetResourceClassHash();
        return alloc;
    }
    auto & desc = buffer->desc_;
    desc.size = RDGBuffer::GetBestAllocationSizeFromRequestedSize(requested_size);
    if (desc.size <= kBufferBlockSize) {
        while (true) {
            auto hash = RDGBuffer::GetResourceClassHash(desc, false);
            auto & slot = free_buffer_allocations_[hash];
            if (!slot.empty()) {
                    // Found one, allocate it
                auto alloc = slot.back();
                slot.pop_back();
                return alloc;
            }
            if (log2(desc.size / requested_size) >= kBufferReusingThresholdLog2
                || desc.size > kBufferReusingAbsoluteThreshold) {
                    // Too large to reuse, stop searching and allocate a new buffer block
                break;
            }
            desc.size *= 2;
        }
    }
    // Allocate a new buffer block if no reusable buffer is found or the requested size is larger than the block size.
    auto * alloc = AllocateNewBufferBlock(desc);
    alloc->resource_class_hash = buffer->GetResourceClassHash();
    return alloc;
}

// --- Texture allocation ---

RDGPoolTextureAllocation * RDGResourcePool::AllocateNewTextureBlock(RHITextureDesc desc, uint32_t hash) {
    auto rhi_texture = RHI::Get().CreateTexture(desc);
#if MI_ENABLE_RHI_OBJECT_NAMING
    rhi_texture->SetName("RDGResourcePoolTexture #" + std::to_string(rhi_texture_references_.size()));
#endif
    auto * alloc = new RDGPoolTextureAllocation();
    alloc->texture = rhi_texture.Raw();
    alloc->resource_class_hash = hash;
    total_device_memory_usage_ += rhi_texture->GetSize();
    rhi_texture_references_.emplace_back(std::move(rhi_texture));
    return alloc;
}

// --- Attach ---

void RDGResourcePool::AttachAllocation(RDGBuffer *buffer, RDGPoolBufferAllocation *alloc) {
    if (!buffer->name_.empty() && buffer->dedicated_) {
        alloc->buffer->SetName(buffer->name_);
    }
    alloc->Acquire();
    buffer->allocation_ = alloc;
    buffer->pool_ = this;
    mi_assert(
        !buffer->read_access_ && !buffer->write_access_ && !buffer->read_stages_ && !buffer->write_stages_,
        "Buffer should not have access flags set before allocation."
    );
    // Recover last access within the buffer.
    buffer->read_access_ = alloc->last_access & RHIGPUAccessFlagBits::kRead;
    buffer->write_access_ = alloc->last_access & RHIGPUAccessFlagBits::kWrite;
    buffer->read_stages_ = alloc->last_read_stages;
    buffer->write_stages_ = alloc->last_write_stages;
    num_active_buffers_++;
}

void RDGResourcePool::AttachAllocation(RDGTexture *texture, RDGPoolTextureAllocation *alloc) {
    alloc->Acquire();
    texture->allocation_ = alloc;
    texture->pool_ = this;
    // Recover last access within the texture (mirrors the buffer path). This is the
    // authoritative source for prev-stages/prev-access read by the barrier placement
    // loop (rdg.cpp) and by any manual BufferBarrier/TextureBarrier callers — the
    // per-resource fields must reflect the allocation's tail state right after attach.
    texture->read_access_ = alloc->last_access & RHIGPUAccessFlagBits::kRead;
    texture->write_access_ = alloc->last_access & RHIGPUAccessFlagBits::kWrite;
    texture->read_stages_ = alloc->last_read_stages;
    texture->write_stages_ = alloc->last_write_stages;
    texture->current_layout_ = RHITextureLayoutType::kUndefined;
    num_active_textures_++;
}

// --- Allocate / Recycle ---

void RDGResourcePool::AllocateResource(RDGBuffer *buffer) {
    assert(!buffer->IsImported() && "Imported buffer should not be allocated by the pool.");
    assert(!buffer->IsAllocated() && "This buffer should not be allocated already.");
    assert(buffer->requested_size_ > 0 && "Buffer size should be greater than 0.");
    AttachAllocation(buffer, FindOrCreateBufferAllocation(buffer));
}

void RDGResourcePool::RecycleResource(RDGBuffer *buffer) {
    auto * alloc = buffer->allocation_;
    assert(alloc && "RecycleResource called on unallocated buffer");
    alloc->last_read_stages = buffer->read_stages_;
    alloc->last_write_stages = buffer->write_stages_;
    alloc->last_access = buffer->read_access_ | buffer->write_access_;
    if (alloc->Release()) {
        free_buffer_allocations_[alloc->resource_class_hash].push_back(alloc);
    }
    buffer->allocation_ = nullptr;
    buffer->read_access_ = {};
    buffer->write_access_ = {};
    buffer->read_stages_ = {};
    buffer->write_stages_ = {};
    buffer->desc_.size = 0;
    num_active_buffers_--;
}

void RDGResourcePool::AllocateResource(RDGTexture *texture) {
    assert(!texture->IsImported() && "Imported texture should not be allocated by the pool.");
    assert(!texture->IsAllocated() && "This texture should not be allocated already.");
    auto hash = texture->GetResourceClassHash();
    auto & slot = free_texture_allocations_[hash];
    RDGPoolTextureAllocation * alloc = nullptr;
    if (slot.empty()) {
        alloc = AllocateNewTextureBlock(texture->GetDesc(), hash);
    } else {
        alloc = slot.back();
        slot.pop_back();
    }
    AttachAllocation(texture, alloc);
}

void RDGResourcePool::RecycleResource(RDGTexture *texture) {
    auto * alloc = texture->allocation_;
    assert(alloc && "RecycleResource called on unallocated texture");
    alloc->last_read_stages = texture->read_stages_;
    alloc->last_write_stages = texture->write_stages_;
    alloc->last_access = texture->read_access_ | texture->write_access_;
    if (alloc->Release()) {
        free_texture_allocations_[alloc->resource_class_hash].push_back(alloc);
    }
    texture->allocation_ = nullptr;
    texture->read_access_ = {};
    texture->write_access_ = {};
    texture->read_stages_ = {};
    texture->write_stages_ = {};
    texture->current_layout_ = RHITextureLayoutType::kUndefined;
    num_active_textures_--;
}

// --- Two-phase allocation ---

void RDGResourcePool::RequestAllocation(RDGBuffer *buffer, uint32_t first_pass, uint32_t last_pass) {
    assert(!buffer->IsImported() && "Imported buffer should not request allocation.");
    assert(!buffer->IsAllocated() && "Buffer already allocated.");
    if (buffer->GetFlags() & RDGResourceFlagBits::kExport) {
        AllocateResource(buffer);
        return;
    }
    pending_buffer_allocations_.push_back({buffer, first_pass, last_pass});
}

void RDGResourcePool::RequestAllocation(RDGTexture *texture, uint32_t first_pass, uint32_t last_pass) {
    assert(!texture->IsImported() && "Imported texture should not request allocation.");
    assert(!texture->IsAllocated() && "Texture already allocated.");
    if (texture->GetFlags() & RDGResourceFlagBits::kExport) {
        AllocateResource(texture);
        return;
    }
    pending_texture_allocations_.push_back({texture, first_pass, last_pass});
}

void RDGResourcePool::CommitAllocations() {
    // --- Buffer aliasing ---
    std::stable_sort(pending_buffer_allocations_.begin(), pending_buffer_allocations_.end(),
        [](const PendingBufferAllocation & a, const PendingBufferAllocation & b) {
            return a.buffer->GetResourceClassHash() < b.buffer->GetResourceClassHash();
        });

    struct AliasedSlot {
        RDGPoolBufferAllocation * allocation;
        uint32_t last_pass;
    };

    size_t i = 0;
    while (i < pending_buffer_allocations_.size()) {
        uint32_t group_hash = pending_buffer_allocations_[i].buffer->GetResourceClassHash();
        size_t group_start = i;
        while (i < pending_buffer_allocations_.size() && pending_buffer_allocations_[i].buffer->GetResourceClassHash() == group_hash) {
            i++;
        }
        size_t group_end = i;

        std::sort(pending_buffer_allocations_.begin() + group_start, pending_buffer_allocations_.begin() + group_end,
            [](const PendingBufferAllocation & a, const PendingBufferAllocation & b) {
                return a.first_pass < b.first_pass;
            });

        std::vector<AliasedSlot> aliased_slots;

        for (size_t j = group_start; j < group_end; j++) {
            auto & pending = pending_buffer_allocations_[j];
            auto * buffer = pending.buffer;

            AliasedSlot * best = nullptr;
            for (auto & slot : aliased_slots) {
                if (slot.allocation && slot.last_pass < pending.first_pass) {
                    if (!best || slot.last_pass > best->last_pass) {
                        best = &slot;
                    }
                }
            }

            if (best) {
                AttachAllocation(buffer, best->allocation);
                best->last_pass = pending.last_pass;
            } else {
                auto * alloc = FindOrCreateBufferAllocation(buffer);
                AttachAllocation(buffer, alloc);
                aliased_slots.push_back({alloc, pending.last_pass});
            }
        }
    }

    // --- Texture aliasing (mirrors the buffer algorithm above) ---
    // Transient textures with non-overlapping lifetimes share one physical allocation.
    // No extra state is needed for the layout handoff: the successor's first use transitions
    // UNDEFINED -> its required layout (discarding the predecessor's contents, which it does
    // not need), while the memory-access barrier is inherited from alloc->last_access.
    std::stable_sort(pending_texture_allocations_.begin(), pending_texture_allocations_.end(),
        [](const PendingTextureAllocation & a, const PendingTextureAllocation & b) {
            return a.texture->GetResourceClassHash() < b.texture->GetResourceClassHash();
        });

    struct AliasedTextureSlot {
        RDGPoolTextureAllocation * allocation;
        uint32_t last_pass;
    };

    for (size_t ti = 0; ti < pending_texture_allocations_.size(); ) {
        uint32_t group_hash = pending_texture_allocations_[ti].texture->GetResourceClassHash();
        size_t group_start = ti;
        while (ti < pending_texture_allocations_.size() && pending_texture_allocations_[ti].texture->GetResourceClassHash() == group_hash) {
            ti++;
        }
        size_t group_end = ti;

        std::sort(pending_texture_allocations_.begin() + group_start, pending_texture_allocations_.begin() + group_end,
            [](const PendingTextureAllocation & a, const PendingTextureAllocation & b) {
                return a.first_pass < b.first_pass;
            });

        std::vector<AliasedTextureSlot> aliased_slots;

        for (size_t j = group_start; j < group_end; j++) {
            auto & pending = pending_texture_allocations_[j];
            auto * texture = pending.texture;

            AliasedTextureSlot * best = nullptr;
            for (auto & slot : aliased_slots) {
                if (slot.allocation && slot.last_pass < pending.first_pass) {
                    if (!best || slot.last_pass > best->last_pass) {
                        best = &slot;
                    }
                }
            }

            if (best) {
                AttachAllocation(texture, best->allocation);
                best->last_pass = pending.last_pass;
            } else {
                // No reusable slot within this frame. AllocateResource itself recycles from the
                // cross-frame free list first, then falls back to creating a new physical texture.
                AllocateResource(texture);
                aliased_slots.push_back({texture->allocation_, pending.last_pass});
            }
        }
    }

    pending_buffer_allocations_.clear();
    pending_texture_allocations_.clear();
}

MI_NAMESPACE_END
