/*
 * Created: 2024/9/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VK_CONSTANTS_H
#define MI_VK_CONSTANTS_H

#include "vk_rhi.h"
MI_NAMESPACE_BEGIN

namespace C {
    constexpr uint32_t kMaxNumDescriptorSetsPerFrame = 512;
    constexpr uint32_t kMaxNumUniformBufferDescriptorsPerFrame = 2048;
    constexpr uint32_t kMaxNumStorageBufferDescriptorsPerFrame = 2048;
    constexpr uint32_t kMaxNumSampledTextureDescriptorsPerFrame = 8192;
    constexpr uint32_t kMaxNumStorageTextureDescriptorsPerFrame = 8192;
    constexpr uint32_t kMaxNumSamplerDescriptorsPerFrame = 32;
    constexpr uint32_t kMaxNumAccelerationStructureDescriptorsPerFrame = 32;
}

MI_NAMESPACE_END
#endif //MI_VK_CONSTANTS_H
