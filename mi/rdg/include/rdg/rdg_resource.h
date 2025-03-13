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
    friend class RDGResourcePool;
    FORCEINLINE RDGTexture (RDGResourcePool * pool, RHITextureDesc desc) : RDGResource(pool), desc_(desc) {}
    ~RDGTexture () override ;
    FORCEINLINE RHITextureDesc GetDesc () const { return desc_; }
    uint32_t GetResourceClassHash () const override;
    void RequestRHI() override;
    void ReleaseRHI() override;
    FORCEINLINE bool IsAllocated () const { return rhi_texture_ != nullptr; }
protected:
    RHITextureDesc desc_ {};
    // Underlying RHI texture, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHITexture * rhi_texture_ {};
};

class RDGBuffer : public RDGResource {
public:
    friend class RDGResourcePool;
    FORCEINLINE RDGBuffer (RDGResourcePool * pool, RHIBufferUsageFlags usage, size_t size) : RDGResource(pool), desc_({size, usage}) {}
    FORCEINLINE void SetDedicated (bool value = true) {
        assert(!rhi_buffer_span_.buffer && "Cannot set dedicated flag after buffer allocation.");
        dedicated_ = value;
    }
    ~RDGBuffer () override ;
    uint32_t GetResourceClassHash () const override;
    FORCEINLINE size_t GetSize () const { return desc_.size; }
    FORCEINLINE RHIBufferUsageFlags GetUsage () const { return desc_.usage; }
    FORCEINLINE RHIBufferDesc GetDesc () const { return desc_; }
    void RequestRHI() override;
    void ReleaseRHI() override;
    FORCEINLINE bool IsAllocated () const { return rhi_buffer_span_.buffer != nullptr; }
protected:
    // If true, RDG resource pool tends to map the buffer to a dedicated RHI buffer.
    // when set, rhi_buffer_span_ should have 0 offset.
    bool dedicated_ {};
    RHIBufferDesc desc_ {};
    // Underlying RHI buffer, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHIBufferSpan rhi_buffer_span_ {};
};

MI_NAMESPACE_END
#endif //MI_RDG_RESOURCE_H
