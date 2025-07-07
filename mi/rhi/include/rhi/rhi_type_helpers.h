/*
 * Created: 2025/7/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RHI_TYPE_HELPERS_H
#define RHI_TYPE_HELPERS_H

#include <rhi/rhi_types.h>
MI_NAMESPACE_BEGIN

FORCEINLINE RHIGPUAccessFlags GetReadAccessFlags(RHIGPUAccessFlags access) {
    return access & RHIGPUAccessFlagBits::kRead;
}

FORCEINLINE RHIGPUAccessFlags GetWriteAccessFlags(RHIGPUAccessFlags access) {
    return access & RHIGPUAccessFlagBits::kWrite;
}

MI_NAMESPACE_END

#endif //RHI_TYPE_HELPERS_H
