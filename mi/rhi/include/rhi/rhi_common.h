/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERERDEV_RHI_COMMON_H
#define MIRENDERERDEV_RHI_COMMON_H

#include "core/common.h"

MI_NAMESPACE_BEGIN

// Maximum number of bindless resource slot reserved for each type of RHIBindlessResourceType
constexpr uint32_t CRHIMaxBindlessSlotsPerResourceType = 4096;

// Preferred size of GPU heap blocks. Larger values may increase VRAM consumption.
constexpr uint64_t CRHIPreferredGPUHeapBlockSize = 256 * 1024 * 1024; // 256MB

MI_NAMESPACE_END

#endif //MIRENDERERDEV_RHI_COMMON_H
