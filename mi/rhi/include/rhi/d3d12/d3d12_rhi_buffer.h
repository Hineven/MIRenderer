/*
 * Created: 2025/2/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef D3D12_RHI_BUFFER_H
#define D3D12_RHI_BUFFER_H

#include "rhi/rhi_buffer.h"

MI_NAMESPACE_BEGIN

class D3D12RHIBuffer : public RHIBuffer
{
public:
    D3D12RHIBuffer (size_t buffer_size, RHIBufferUsageFlags usage) ;
    virtual ~D3D12RHIBuffer () override ;
    void * Map () ;
    void Unmap () ;
}

MI_NAMESPACE_END

#endif //D3D12_RHI_BUFFER_H
