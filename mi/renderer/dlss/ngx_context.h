/*
 * Created: 2026/05/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_NGX_CONTEXT_H
#define MI_NGX_CONTEXT_H

#include <vector>
#include "core/common.h"
#include "core/refcounted.h"

MI_NAMESPACE_BEGIN

// Global NGX SDK context. One per process / Renderer.
// Responsible for NVSDK_NGX_VULKAN_Init_with_ProjectID / Shutdown1 only.
class NGXContext : public RefCounted<> {
public:
    static bool ProbeAvailability();

    // Query Vulkan instance/device extensions required by NGX.
    // Should be called before RHI::InitializeSingleton(RHIType::kVulkan).
    static void GetRequiredVulkanExtensions(std::vector<const char*>& out_instance_extensions,
                                            std::vector<const char*>& out_device_extensions);

    NGXContext() = default;
    ~NGXContext();

    bool Initialize();
    void Shutdown();

    FORCEINLINE bool IsInitialized() const { return is_initialized_; }

private:
    bool is_initialized_ = false;
};

MI_NAMESPACE_END

#endif
