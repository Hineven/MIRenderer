/*
 * Created: 2024/7/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_BUFFER_H
#define MI_RHI_BUFFER_H

#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
#include "rhi/rhi_resource.h"
#include "rhi/rhi_bindlesskeeper.h"

MI_NAMESPACE_BEGIN

class RHIBuffer : public RHIResource {
protected:
    // You can only create buffers via factory functions in the RHI instance
    inline RHIBuffer (size_t buffer_size, RHIBufferUsageFlags usage) : buffer_size_(buffer_size), usage_(usage) {}
    virtual ~RHIBuffer() = default;

public:
    FORCEINLINE size_t GetBufferSize () const {return buffer_size_;}
    FORCEINLINE RHIBufferUsageFlags GetBufferUsage () const {return usage_;}

    // Only buffers that are created with the RHIBufferType::kStaging type can be mapped
    virtual void * Map () = 0;
    // Unmap the buffer
    virtual void Unmap () = 0;

    FORCEINLINE bool IsMapped () const {return is_mapped_;}

    FORCEINLINE RHIBufferSpan GetSpan (size_t offset = 0, size_t size = 0) {
        return {this, offset, size == 0 ? buffer_size_ : size};
    }

    // View it as a storage buffer or uniform buffer.
    void ConvertToBindless (bool read_only) ;

    FORCEINLINE RHIBindlessSlotRef<RHIBuffer> GetBindlessSlotReadonly() { return bindless_slot_readonly_; }
    FORCEINLINE RHIBindlessSlotRef<RHIBuffer> GetBindlessSlotReadwrite() { return bindless_slot_readwrite_; }

protected:
    size_t buffer_size_;
    RHIBufferUsageFlags usage_;

    RHIBindlessSlotRef<RHIBuffer> bindless_slot_readonly_ {};
    RHIBindlessSlotRef<RHIBuffer> bindless_slot_readwrite_ {};

    bool is_mapped_ {false};
};

MI_NAMESPACE_END

#endif //MI_RHI_BUFFER_H
