/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <xxhash.h>
#include "rdg/rdg_resource.h"

#include <core/crc.h>
#include <rdg/rdg_pool.h>
MI_NAMESPACE_BEGIN

RDGTexture::~RDGTexture() {
    // Ref count approaching zero, recycle the resource and release corresponding RHI resource.
    RDGTexture::ReleaseRHI();
}
RDGBuffer::~RDGBuffer() {
    // Ref count approaching zero, recycle the resource and release corresponding RHI resource.
    RDGBuffer::ReleaseRHI();
}

void RDGTexture::RequestRHI() {
    if (!rhi_texture_) {
        pool_->AllocateResource(this);
    }
}
void RDGTexture::ReleaseRHI() {
    if (rhi_texture_) {
        pool_->RecycleResource(this);
        rhi_texture_ = nullptr;
    }
}
void RDGBuffer::RequestRHI() {
    if (!rhi_buffer_span_.buffer) {
        pool_->AllocateResource(this);
    }
}
void RDGBuffer::ReleaseRHI() {
    if (rhi_buffer_span_.buffer) {
        pool_->RecycleResource(this);
        rhi_buffer_span_ = {};
    }
}

uint32_t RDGBuffer::GetResourceClassHash() const {
    if (!dedicated_) {
        // Minimum class is 1k bytes
        auto log2size = std::max((uint32_t)log2(desc_.size), 10u) - 10u;
        return CRC32(&desc_.usage, sizeof(desc_.usage), CRC32(&log2size, sizeof(log2size)));
    } else {
        // Dedicated allocation should exactly match the size and usage
        return CRC32(&desc_, sizeof(desc_), 71893718u);
    }
}
uint32_t RDGTexture::GetResourceClassHash() const {
    // Strictly classify them by size and usage
    return CRC32(&desc_, sizeof(desc_));
}

MI_NAMESPACE_END