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

MI_NAMESPACE_BEGIN

class RHIBuffer : public RHIResource {
public:
    inline RHIBuffer (size_t buffer_size, RHIBufferUsageFlags usage, RHIGPUAccessFlagBits access) : buffer_size_(buffer_size), usage_(usage), access_(access) {}
    virtual ~RHIBuffer() = default;

    FORCEINLINE size_t GetBufferSize () const {return buffer_size_;}
    FORCEINLINE RHIBufferUsageFlags GetBufferUsage () const {return usage_;}
    FORCEINLINE RHIGPUAccessFlagBits GetGPUAccess () const {return access_;}

    // Only buffers that are created with the RHIBufferType::kStaging type can be mapped
    virtual void * Map () = 0;
    // Unmap the buffer
    virtual void Unmap () = 0;

    FORCEINLINE bool IsMapped () const {return is_mapped_;}

    FORCEINLINE RHIBufferSpan GetSpan (size_t offset, size_t size) {
        return {this, offset, size};
    }

    void ConvertToBindless (bool read_only) ;

protected:
    size_t buffer_size_;
    RHIBufferUsageFlags usage_;
    RHIGPUAccessFlagBits access_;

    bool is_mapped_ {false};
};

MI_NAMESPACE_END

#endif //MI_RHI_BUFFER_H
