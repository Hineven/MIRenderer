/*
 * Created: 2025/2/22
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rhi/d3d12/d3d12_rhi_export.h"
#include "rhi/d3d12/d3d12_rhi_internal.h"
MI_NAMESPACE_BEGIN

RHIType D3D12RHI::GetType() const {
    return RHIType::kD3D12;
}

const char * D3D12RHI::GetName () const {
    return "Direct3D 12";
}

RHIBufferRef D3D12RHI::CreateBuffer (size_t size, RHIBufferUsageFlagBits type) {
    RHIBuffer
}


MI_NAMESPACE_END