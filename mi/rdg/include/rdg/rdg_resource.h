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
#include "rhi/rhi_desc.h"
#include "rhi/rhi_texture.h"
#include "core/pixel_format.h"

MI_NAMESPACE_BEGIN

class RDGTexture : public RDGResource {
    FORCEINLINE RDGTexture (RHITextureDesc desc) : desc_(desc) {}
public:
    friend class RDGResourcePool;
    friend class RenderGraphBuilder;
    friend class RenderGraph;
    ~RDGTexture () override ;
    FORCEINLINE RHITextureDesc GetDesc () const { return desc_; }
    uint32_t GetResourceClassHash () const override;
    void RequestRHI(RDGResourcePool * pool) override;
    void ReleaseRHI() override;
    FORCEINLINE bool IsAllocated () const { return allocation_ != nullptr; }
    FORCEINLINE RHITexture * GetRHI () const { return allocation_ ? allocation_->texture : nullptr; }

    FORCEINLINE RHIPipelineStageFlags GetReadStages () const { return read_stages_; }
    FORCEINLINE RHIPipelineStageFlags GetWriteStages () const {return write_stages_;}

    // Update RDG resource tracking after manual RHI usage. If you're using it as RDG
    // shader parameters or added it to pass resource access, you don't need to call this explicitly.
    FORCEINLINE void Use (RHIPipelineStageFlags stages, RHIGPUAccessFlags usage, RHITextureLayoutType layout = RHITextureLayoutType::kUndefined) {
        RDGResource::Use(stages, usage);
        if (layout != RHITextureLayoutType::kUndefined) {
            // If the layout is specified, we assume that the texture will be used in this layout.
            // This is useful for textures that are used in a specific layout, such as depth textures.
            current_layout_ = layout;
        }
    }

    FORCEINLINE RHITextureLayoutType GetCurrentLayout () const { return current_layout_; }

    FORCEINLINE bool IsImportedFrom (RHITexture * texture) {
        mi_assert(IsImported(), "This should be an imported texture to call RDGTexture::IsImportedFrom().");
        return GetRHI() == texture;
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

    FORCEINLINE static TRef<RDGTexture> Create2DArray (
    uint32_t width, uint32_t height, uint32_t layers, PixelFormatType format,
    RHITextureUsageFlags usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess) {
        return Create(RHITextureDesc{
            RHITextureType::k2DArray,
            {width, height, 1},
            1, layers, format, usage
        });
    }

    FORCEINLINE static TRef<RDGTexture> Create3D (
    uint32_t width, uint32_t height, uint32_t depth, PixelFormatType format,
    RHITextureUsageFlags usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess,
    uint32_t mip_levels = 1) {
        return Create(RHITextureDesc{
            RHITextureType::k3D,
            {width, height, depth},
            mip_levels, 1, format, usage // 3D Textures usually have 1 array layer
        });
    }

protected:

    RHITextureDesc desc_ {};
    // Underlying RDG pool allocation, can be null if not allocated.
    RDGPoolTextureAllocation * allocation_ {};
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
    friend class RenderGraph;

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
        assert(!allocation_ && "Cannot set dedicated flag after buffer allocation.");
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
    FORCEINLINE bool IsAllocated () const { return allocation_ != nullptr; }
    FORCEINLINE RHIBufferSpan GetRHI () const { return allocation_ ? RHIBufferSpan{allocation_->buffer, 0, requested_size_} : RHIBufferSpan{}; }

    FORCEINLINE bool IsImportedFrom (RHIBufferSpan buffer) {
        mi_assert(IsImported(), "This should be an imported buffer to call RDGBuffer::IsImportedFrom().");
        return GetRHI() == buffer;
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
    // Underlying RDG pool allocation, can be null if not allocated.
    RDGPoolBufferAllocation * allocation_ {};

    // Name of the buffer, used for debug tracking
    std::string name_ {};
};

typedef TRef<RDGTexture> RDGTextureRef;
typedef TRef<RDGBuffer> RDGBufferRef;

MI_NAMESPACE_END
#endif //MI_RDG_RESOURCE_H
