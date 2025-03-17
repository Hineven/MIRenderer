/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RDG_RESOURCE_H
#define MI_RDG_RESOURCE_H

#include <core/crc.h>

#include "rhi/rhi.h"
#include "rdg/rdg_base.h"
#include "rhi/rhi_texture.h"
#include "core/pixel_format.h"

MI_NAMESPACE_BEGIN

class RDGTexture : public RDGResource {
public:
    friend class RDGResourcePool;
    FORCEINLINE RDGTexture (RHITextureDesc desc) : desc_(desc) {}
    ~RDGTexture () override ;
    FORCEINLINE RHITextureDesc GetDesc () const { return desc_; }
    uint32_t GetResourceClassHash () const override;
    void RequestRHI(RDGResourcePool * pool) override;
    void ReleaseRHI() override;
    FORCEINLINE bool IsAllocated () const { return rhi_texture_ != nullptr; }
    FORCEINLINE RHITexture * GetRHI () const { return rhi_texture_; }
protected:
    RHITextureDesc desc_ {};
    // Underlying RHI texture, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHITexture * rhi_texture_ {};
};

class RDGBuffer : public RDGResource {
public:
    constexpr static uint32_t kMinBufferSizeLog2 = 10;
    constexpr static uint32_t kMinBufferSize = 1 << kMinBufferSizeLog2;
    FORCEINLINE static uint32_t GetResourceClassHash (RHIBufferDesc desc, bool dedicated) {
        if (!dedicated) {
            // Minimum class is 1k bytes
            auto log2size = std::max((uint32_t)log2(desc.size), kMinBufferSizeLog2) - kMinBufferSizeLog2;
            return CRC32(&desc.usage, sizeof(desc.usage), CRC32(&log2size, sizeof(log2size)));
        } else {
            // Dedicated allocation should exactly match the size and usage
            return CRC32(&desc, sizeof(desc), 71893718u);
        }
    }

    friend class RDGResourcePool;
    FORCEINLINE RDGBuffer (RHIBufferUsageFlags usage, size_t size) : desc_({size, usage}) {}
    FORCEINLINE void SetDedicated (bool value = true) {
        assert(!rhi_buffer_span_.buffer && "Cannot set dedicated flag after buffer allocation.");
        dedicated_ = value;
    }
    ~RDGBuffer () override ;
    uint32_t GetResourceClassHash () const override;
    FORCEINLINE size_t GetSize () const { return desc_.size; }
    FORCEINLINE RHIBufferUsageFlags GetUsage () const { return desc_.usage; }
    FORCEINLINE RHIBufferDesc GetDesc () const { return desc_; }
    void RequestRHI(RDGResourcePool * pool) override;
    void ReleaseRHI() override;
    FORCEINLINE bool IsAllocated () const { return rhi_buffer_span_.buffer != nullptr; }
    FORCEINLINE RHIBufferSpan GetRHI () const { return rhi_buffer_span_; }
protected:
    // If true, RDG resource pool tends to map the buffer to a dedicated RHI buffer.
    // when set, rhi_buffer_span_ should have 0 offset.
    bool dedicated_ {};
    RHIBufferDesc desc_ {};
    // Underlying RHI buffer, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHIBufferSpan rhi_buffer_span_ {};
};

typedef TRef<RDGTexture> RDGTextureRef;
typedef TRef<RDGBuffer> RDGBufferRef;

MI_NAMESPACE_END
#endif //MI_RDG_RESOURCE_H
