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
    // Ref count approaching zero, recycle the resource and release corresponding RHI resource.
    RDGBuffer::ReleaseRHI();
}

void RDGTexture::RequestRHI(RDGResourcePool * pool) {
    if (!IsImported()) {
        if (!rhi_texture_) {
            pool_ = pool;
            pool_->AllocateResource(this);
        } else {
            assert(pool == pool_ && "Re-allocating RDG resources from different pools is not allowed.");
        }
    }
}
void RDGTexture::ReleaseRHI() {
    if (!IsImported() && rhi_texture_) {
        pool_->RecycleResource(this);
        rhi_texture_ = nullptr;
        pool_ = nullptr;
    }
}
void RDGBuffer::RequestRHI(RDGResourcePool * pool) {
    if (!IsImported()) {
        pool_ = pool;
        if (!rhi_buffer_span_.buffer && requested_size_ > 0) {
            pool_->AllocateResource(this);
        } else {
            assert(pool == pool_ && "Re-allocating RDG resources from different pools is not allowed.");
        }
    }
}
void RDGBuffer::ReleaseRHI() {
    if (!IsImported() && rhi_buffer_span_.buffer) {
        pool_->RecycleResource(this);
        rhi_buffer_span_ = {};
    }
    pool_ = nullptr;
}

void *RDGBuffer::Map() const {
    assert(IsAllocated() && "Buffer must be allocated before mapping.");
    return (std::byte*)rhi_buffer_span_.buffer->Map() + rhi_buffer_span_.offset;
}

uint32_t RDGBuffer::GetResourceClassHash () const {
    return RDGBuffer::GetResourceClassHash(desc_, dedicated_);
}
uint32_t RDGTexture::GetResourceClassHash() const {
    // Strictly classify them by size and usage
    return CRC32(&desc_, sizeof(desc_));
}

TRef<RDGTexture> RDGTexture::Import([[maybe_unused]] const char * name, RHITexture * resource, RDGTextureUsageType prev_usage) {
    auto texture_raw_ptr = new RDGTexture(resource->GetDesc());
    auto texture = TRef<RDGTexture>(texture_raw_ptr);
    texture->rhi_texture_ = resource;
    texture->usage_ = prev_usage;
    texture->flags_ = RDGResourceFlagBits::kImported | RDGResourceFlagBits::kPersistent;
    return texture;
}

TRef<RDGBuffer> RDGBuffer::Import([[maybe_unused]] const char *name, RHIBuffer * resource, RHIGPUAccessFlags prev_access) {
    auto desc = resource->GetDesc();
    auto buffer_raw_ptr = new RDGBuffer(desc.size, desc);
    auto buffer = TRef<RDGBuffer>(buffer_raw_ptr);
    buffer->rhi_buffer_span_ = resource->GetSpan();
    buffer->usage_ = prev_access;
    buffer->flags_ = RDGResourceFlagBits::kImported | RDGResourceFlagBits::kPersistent;
    return buffer;
}


MI_NAMESPACE_END