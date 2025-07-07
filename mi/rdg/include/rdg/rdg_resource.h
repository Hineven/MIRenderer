/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RDG_RESOURCE_H
#define MI_RDG_RESOURCE_H

#include <corecrt_io.h>
#include <core/crc.h>
#include <rhi/rhi_type_helpers.h>
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
    ~RDGTexture () override ;
    FORCEINLINE RHITextureDesc GetDesc () const { return desc_; }
    uint32_t GetResourceClassHash () const override;
    void RequestRHI(RDGResourcePool * pool) override;
    void ReleaseRHI() override;
    FORCEINLINE bool IsAllocated () const { return rhi_texture_ != nullptr; }
    FORCEINLINE RHITexture * GetRHI () const { return rhi_texture_; }

    FORCEINLINE RHIPipelineStageFlags GetReadStages () const { return read_stages_; }
    FORCEINLINE RHIPipelineStageFlags GetWriteStages () const {return write_stages_;}
    FORCEINLINE void Use (RHIPipelineStageFlags stages, RHIGPUAccessFlags usage, RHITextureLayoutType layout = RHITextureLayoutType::kUndefined) {
        RDGResource::Use(stages, usage);
        if (layout != RHITextureLayoutType::kUndefined) {
            // If the layout is specified, we assume that the texture will be used in this layout.
            // This is useful for textures that are used in a specific layout, such as depth textures.
            current_layout_ = layout;
        }
    }

    FORCEINLINE bool IsImportedFrom (RHITexture * texture) {
        mi_assert(IsImported(), "This should be an imported texture to call RDGTexture::IsImportedFrom().");
        return rhi_texture_ == texture;
    }

    FORCEINLINE const std::string & GetName () const {
        return name_;
    }
    FORCEINLINE void SetName (const std::string & name) {
        name_ = name;
    }


    // Create a RDG texture with the given description.
    FORCEINLINE static TRef<RDGTexture> Create (RHITextureDesc desc) {
        return {new RDGTexture(desc)};
    }

    FORCEINLINE static TRef<RDGTexture> Create2D (
    uint32_t width, uint32_t height, PixelFormatType format,
    RHITextureUsageFlags usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess) {
        return Create(RHITextureDesc{
            RHITextureType::k2D,
            {width, height, 1},
            1, 1, format, usage
        });
    }

protected:

    RHITextureDesc desc_ {};
    // Underlying RHI texture, can be null if not allocated.
    // The reference is kept by RDG resource pool, we'll just use plain pointer here.
    RHITexture * rhi_texture_ {};
    // Current layout of the texture, used for barrier placement.
    RHITextureLayoutType current_layout_ {RHITextureLayoutType::kUndefined};

    std::string name_ {};
};

class RDGBuffer : public RDGResource {
protected:
    FORCEINLINE RDGBuffer(size_t req_size, RHIBufferDesc desc, bool dedicated = false, bool no_warning = false) :
        requested_size_(req_size), desc_(desc), dedicated_(dedicated) {
        if (!no_warning && ((desc.usage & RHIBufferUsageFlagBits::kStaging) || (desc.usage & RHIBufferUsageFlagBits::kReadback))) {
            MI_LOG(MIInfraLogType::kWarning, "We suggest using RHI directly with staging and readback buffers (fire and forgot, "
                                             "RHI will take care of safe recycling)."
                                             "Otherwise you must carefully handle their lifetimes when performing GPU-CPU data-transactions.");
        }
    }
    FORCEINLINE RDGBuffer (size_t req_size, RHIBufferUsageFlags usage, size_t size, bool dedicated = false, bool no_warning = false):
        RDGBuffer(req_size, RHIBufferDesc{ size, usage}, dedicated, no_warning) {}
public:
    friend class RDGResourcePool;
    friend class RenderGraphBuilder;

    FORCEINLINE void SetName (const std::string & name) {name_ = name;}
    FORCEINLINE std::string GetName () const { return name_;}

    FORCEINLINE static size_t GetBestAllocationSizeFromRequestedSize (size_t requested_size) {
        int l = std::max((int)std::ceil(log2(requested_size)), (int)kMinBufferSizeLog2);
        if (l > RDGResourcePool::kBufferBlockSizeLog2) {
            return requested_size;
        }
        return 1ull << l;
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

    FORCEINLINE bool IsImportedFrom (RHIBufferSpan buffer) {
        mi_assert(IsImported(), "This should be an imported buffer to call RDGBuffer::IsImportedFrom().");
        return rhi_buffer_span_ == buffer;
    }

    // Short hand for (std::byte*)GetRHI().buffer->Map() + GetRHI().offset
    void * Map () const ;

    FORCEINLINE static TRef<RDGBuffer> Create (RHIBufferUsageFlags usage, size_t size,
        bool dedicated = false, bool no_warning = false) {
        return {new RDGBuffer(GetBestAllocationSizeFromRequestedSize(size), usage, size, dedicated, no_warning)};
    }


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

    // Name of the buffer, used for debug tracking
    std::string name_ {};
};

typedef TRef<RDGTexture> RDGTextureRef;
typedef TRef<RDGBuffer> RDGBufferRef;

MI_NAMESPACE_END
#endif //MI_RDG_RESOURCE_H
