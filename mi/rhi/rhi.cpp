/*
 * Created: 2024/7/7
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "rhi/rhi.h"
#include "rhi_cmd_exec.h"

// Import different kinds of RHI implementations
#include "vk/vk_rhi_export.h"

MI_NAMESPACE_BEGIN

static RHI * GDynamicRHI = nullptr;
static thread_local bool GIsRhiThread = false;

RHI & RHI::GetInstance () {
    if(!GDynamicRHI) {
        mi_assert(false, "RHI instance not created");
    }
    return *GDynamicRHI;
}

bool IsRHIThread () {
    return GIsRhiThread;
}

void SetIsRHIThread (bool is_rhi_thread) {
    GIsRhiThread = is_rhi_thread;
}

void RHIInitialize (RHIType type) {
    if(GDynamicRHI) {
        mi_assert(false, "RHI instance already created");
    }
    switch (type) {
        case RHIType::kVulkan:
            GDynamicRHI = reinterpret_cast<RHI *>(CreateVulkanRHIInstance());
            break;
        // ...
        default:
            mi_assert(false, "Unknown RHI type");
    }
}

void RHIDestroy () {
    if(GDynamicRHI) {
        delete GDynamicRHI;
        GDynamicRHI = nullptr;
    }
}

MI_NAMESPACE_END
