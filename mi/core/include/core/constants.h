/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CONSTANTS_H
#define MIRENDERER_CONSTANTS_H

#include "core/common.h"
MI_NAMESPACE_BEGIN

namespace C {
    constexpr uint32_t kMaxTaskGraphTaskCount   = 1024;
    constexpr uint32_t kMaxTaskGraphThreadCount = 64;
    constexpr uint32_t kNumDefaultBindlessImmutableSamplers = 4;
    // We don't need plentiful bindless acceleration structures
    constexpr uint32_t kMaxNumBindlessAccelerationStructures = 16;
    constexpr uint32_t kMaxNumBindlessResourceSlotsPerChannel = 1024;


    // Preferred size of GPU heap blocks. Larger values may increase VRAM consumption.
    constexpr uint64_t kRHIPreferredGPUHeapBlockSize = 256 * 1024 * 1024; // 256MB
    // Maximum number of framebuffer attachments
    constexpr uint32_t kRHIMaxNumFramebufferAttachments = 4;
}

MI_NAMESPACE_END
#endif //MIRENDERER_CONSTANTS_H
