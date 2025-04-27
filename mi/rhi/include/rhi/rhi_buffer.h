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
    RHIBuffer (RHIBufferDesc desc) : desc_(desc) {}
    virtual ~RHIBuffer() = default;

public:
    FORCEINLINE size_t GetBufferSize () const {return desc_.size;}
    FORCEINLINE RHIBufferUsageFlags GetBufferUsage () const {return desc_.usage;}

    // Only buffers that are created with the RHIBufferType::kStaging / kReadback type can be mapped
    virtual void * Map () = 0;
    // Unmap the buffer
    virtual void Unmap () = 0;

    FORCEINLINE bool IsMapped () const {return is_mapped_;}

    FORCEINLINE RHIBufferSpan GetSpan (size_t offset = 0, size_t size = 0) {
        return {this, offset, size == 0 ? desc_.size : size};
    }

    FORCEINLINE RHIBufferDesc GetDesc () const {return desc_;}

protected:
    RHIBufferDesc desc_;
    bool is_mapped_ {false};
};

MI_NAMESPACE_END

#endif //MI_RHI_BUFFER_H
