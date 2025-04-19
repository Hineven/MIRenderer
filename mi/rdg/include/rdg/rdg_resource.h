/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RDG_RESOURCE_H
#define MI_RDG_RESOURCE_H

#include <core/crc.h>

#include "rdg_pool.h"
#include "rhi/rhi.h"
#include "rdg/rdg_base.h"
#include "rhi/rhi_texture.h"
#include "core/pixel_format.h"

MI_NAMESPACE_BEGIN

class RDGTexture : public RDGResource {
    FORCEINLINE RDGTexture (RHITextureDesc desc) : desc_(desc) {}
public:
    friend class RDGResourcePool;
    friend class RenderGraphBuilder;
    FORCEINLINE static TRef<RDGTexture> Create (RHITextureDesc desc) {
        return TRef<RDGTexture>(new RDGTexture(desc));
    }
    FORCEINLINE static TRef<RDGTexture> CreateTexture2D (
        uint32_t width, uint32_t height, PixelFormatType format,
        RHITextureUsageFlags usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess) {
        return Create(RHITextureDesc{
            RHITextureType::k2D,
            {width, height, 1},
            1, 1, format, usage
        });
    }
    // Import a rhi texture. NOTE: the reference is not kept by RDGTexture, you should manage the lifetime of the resource.
    static TRef<RDGTexture> Import (const char * name, RHITexture * resource, RDGTextureUsageType prev_usage) ;
    // Import a rhi texture. NOTE: the reference is not kept by RDGTexture, you should manage the lifetime of the resource.
    FORCEINLINE static TRef<RDGTexture> Import (RHITexture * resource, RDGTextureUsageType prev_usage = RDGTextureUsageType::kNone) {
        return Import("<unnamed>", resource, prev_usage);
    }
    ~RDGTexture () override ;
    FORCEINLINE RHITextureDesc GetDesc () const { return desc_; }
    uint32_t GetResourceClassHash () const override;
    void RequestRHI(RDGResourcePool * pool) override;
    void ReleaseRHI() override;
    FORCEINLINE bool IsAllocated () const { return rhi_texture_ != nullptr; }
    FORCEINLINE RHITexture * GetRHI () const { return rhi_texture_; }

    FORCEINLINE RDGTextureUsageType GetLastUsage () const { return usage_; }
    FORCEINLINE void Use (RDGTextureUsageType usage) { usage_ = usage; }

protected:
    RHITextureDesc desc_ {};
    // Underlying RHI texture, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHITexture * rhi_texture_ {};
    // Track the last access of the texture, used for barrier placement.
    RDGTextureUsageType usage_ {};
};

class RDGBuffer : public RDGResource {
protected:
    FORCEINLINE RDGBuffer(size_t req_size, RHIBufferDesc desc, bool dedicated = false, bool no_warning = false) :
        requested_size_(req_size), desc_(desc), dedicated_(dedicated) {
        if (!no_warning && ((desc.usage & RHIBufferUsageFlagBits::kStaging) || (desc.usage & RHIBufferUsageFlagBits::kReadback))) {
            MI_LOG(MIInfraLogType::kWarning, "We suggest using RHI directly with staging and readback buffers (fire and forgot)."
                                             "Otherwise you may carefully handle their lifetimes when performing GPU-CPU data-transactions.");
        }
    }
    FORCEINLINE RDGBuffer (size_t req_size, RHIBufferUsageFlags usage, size_t size, bool dedicated = false, bool no_warning = false):
        RDGBuffer(req_size, RHIBufferDesc{ size, usage}, dedicated, no_warning) {}
public:
    friend class RDGResourcePool;
    friend class RenderGraphBuilder;

    FORCEINLINE void SetName (const std::string & name) {name_ = name;}

    FORCEINLINE static size_t GetBestAllocationSizeFromRequestedSize (size_t requested_size) {
        int l = std::max((int)std::ceil(log2(requested_size)), (int)kMinBufferSizeLog2);
        if (l > RDGResourcePool::kBufferBlockSizeLog2) {
            return requested_size;
        }
        return 1ull << l;
    }

    FORCEINLINE static TRef<RDGBuffer> Create (RHIBufferUsageFlags usage, size_t size, bool dedicated = false, bool no_warning = false) {
        return TRef<RDGBuffer>(new RDGBuffer(size, usage, 0, dedicated, no_warning));
    }
    // Import a rhi buffer. NOTE: the reference is not kept by RDGBuffer, you should manage the lifetime of the resource.
    static TRef<RDGBuffer> Import (const char *name, RHIBuffer * resource, RHIGPUAccessFlags prev_access) ;
    // Import a rhi buffer. NOTE: the reference is not kept by RDGBuffer, you should manage the lifetime of the resource.
    FORCEINLINE static TRef<RDGBuffer> Import (RHIBuffer * resource, RHIGPUAccessFlags prev_access = RHIGPUAccessFlagBits::kNone) {
        return Import("<unnamed>", resource, prev_access);
    }

    constexpr static uint32_t kMinBufferSizeLog2 = 10;
    constexpr static uint32_t kMinBufferSize = 1 << kMinBufferSizeLog2;
    FORCEINLINE static uint32_t GetResourceClassHash (RHIBufferDesc desc, [[maybe_unused]] bool dedicated) {
        return CRC32(&desc.usage, sizeof(desc.usage), CRC32(&desc.size, sizeof(desc.size)));
    }

    FORCEINLINE void SetDedicated (bool value = true) {
        assert(!rhi_buffer_span_.buffer && "Cannot set dedicated flag after buffer allocation.");
        dedicated_ = value;
    }
    ~RDGBuffer () override ;
    uint32_t GetResourceClassHash () const override;
    FORCEINLINE size_t GetRequestedSize () const { return requested_size_; }
    FORCEINLINE size_t GetAllocationSize () const { return desc_.size; }
    FORCEINLINE RHIBufferUsageFlags GetUsage () const { return desc_.usage; }
    FORCEINLINE RHIBufferDesc GetDesc () const { return desc_; }
    void RequestRHI(RDGResourcePool * pool) override;
    void ReleaseRHI() override;
    FORCEINLINE bool IsAllocated () const { return rhi_buffer_span_.buffer != nullptr; }
    FORCEINLINE RHIBufferSpan GetRHI () const { return rhi_buffer_span_; }

    FORCEINLINE RHIGPUAccessFlags GetLastUsage () const { return usage_; }
    FORCEINLINE void Use (RHIGPUAccessFlags usage) { usage_ = usage; }

    // Short hand for (std::byte*)GetRHI().buffer->Map() + GetRHI().offset
    void * Map () const ;
protected:
    // If true, RDG resource pool tends to map the buffer to a dedicated RHI buffer.
    // when set, rhi_buffer_span_ should have 0 offset.
    bool dedicated_ {};
    // User requested size, used for buffer allocation, may be smaller than the actual size.
    size_t requested_size_;
    // Real info for potential allocation.
    RHIBufferDesc desc_ {};
    // Underlying RHI buffer, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHIBufferSpan rhi_buffer_span_ {};
    // Track the last access of the buffer, used for barrier placement.
    RHIGPUAccessFlags usage_ {};

    // Name of the buffer, used for debug tracking
    std::string name_ {};
};

typedef TRef<RDGTexture> RDGTextureRef;
typedef TRef<RDGBuffer> RDGBufferRef;

MI_NAMESPACE_END
#endif //MI_RDG_RESOURCE_H
