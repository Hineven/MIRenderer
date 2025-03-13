/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RDG_RESOURCE_H
#define MI_RDG_RESOURCE_H

#include "rhi/rhi.h"
#include "rdg/rdg_base.h"
#include "rhi/rhi_texture.h"
#include "core/pixel_format.h"

MI_NAMESPACE_BEGIN

class RDGTexture : public RDGResource {
public:
    RDGTexture (RHITextureDesc desc) : RDGResource(nullptr), desc_(desc) {}
    ~RDGTexture () override = default;
    FORCEINLINE RHITextureDesc GetDesc () const { return desc_; }
protected:
    RHITextureDesc desc_;
    // Underlying RHI texture, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHITexture * rhi_texture_ {};
};

class RDGBuffer : public RDGResource {
public:
    RDGBuffer (RHIBufferUsageFlags usage, size_t size) : RDGResource(nullptr), desc_({size, usage}) {}
    ~RDGBuffer () override = default;
    FORCEINLINE size_t GetSize () const { return desc_.size; }
    FORCEINLINE RHIBufferUsageFlags GetUsage () const { return desc_.usage; }
    FORCEINLINE RHIBufferDesc GetDesc () const { return desc_; }
protected:
    RHIBufferDesc desc_;
    // Underlying RHI buffer, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHIBufferSpan rhi_buffer_span_;
};

MI_NAMESPACE_END
#endif //MI_RDG_RESOURCE_H
