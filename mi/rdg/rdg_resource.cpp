/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <xxhash.h>
#include "rdg/rdg_resource.h"

#include <core/crc.h>
#include <rdg/rdg_pool.h>
#include <rhi/rhi_buffer.h>
MI_NAMESPACE_BEGIN
RDGResource::RDGResource() {}

RDGResource::~RDGResource() {}

RDGTexture::~RDGTexture() {
    // Ref count approaching zero, recycle the resource and release corresponding RHI resource.
    RDGTexture::ReleaseRHI();
}
RDGBuffer::~RDGBuffer() {
#ifndef NDEBUG
    // Mark as "recycled"
    canary_ = 0x12341234;
#endif
    // Ref count approaching zero, recycle the resource and release corresponding RHI resource.
    RDGBuffer::ReleaseRHI();
}

void RDGTexture::RequestRHI(RDGResourcePool * pool) {
    if (!IsImported()) {
        if (!allocation_) {
            pool_ = pool;
            pool_->AllocateResource(this);
        } else {
            assert(pool == pool_ && "Re-allocating RDG resources from different pools is not allowed.");
        }
    }
}
void RDGTexture::ReleaseRHI() {
    if (!IsImported() && allocation_) {
        pool_->RecycleResource(this);
        allocation_ = nullptr;
        pool_ = nullptr;
    } else if (IsImported() && allocation_) {
        delete allocation_;
        allocation_ = nullptr;
    }
}
void RDGBuffer::RequestRHI(RDGResourcePool * pool) {
    if (!IsImported()) {
        if (!allocation_ && requested_size_ > 0) {
            pool_ = pool;
            pool_->AllocateResource(this);
        } else {
            assert(pool == pool_ && "Re-allocating RDG resources from different pools is not allowed.");
        }
    }
}
void RDGBuffer::ReleaseRHI() {
    if (!IsImported() && allocation_) {
        pool_->RecycleResource(this);
        allocation_ = nullptr;
    } else if (IsImported() && allocation_) {
        delete allocation_;
        allocation_ = nullptr;
    }
    pool_ = nullptr;
}

void *RDGBuffer::Map() const {
    assert(IsAllocated() && "Buffer must be allocated before mapping.");
    auto span = GetRHI();
    return (std::byte*)span.buffer->Map() + span.offset;
}

uint32_t RDGBuffer::GetResourceClassHash () const {
    return RDGBuffer::GetResourceClassHash(desc_, dedicated_);
}
uint32_t RDGTexture::GetResourceClassHash() const {
    // Strictly classify them by size and usage
    return CRC32(&desc_, sizeof(desc_));
}


MI_NAMESPACE_END