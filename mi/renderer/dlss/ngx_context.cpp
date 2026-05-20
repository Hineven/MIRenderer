/*
 * Created: 2026/05/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <windows.h>

#include "ngx_context.h"
#include "rhi/rhi.h"
#include "rhi/rhi_thread.h"
#include "rhi/vk/vk_export.h"

// NGX SDK headers
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

MI_NAMESPACE_BEGIN

static constexpr const char * kDLSSProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";

static std::string NGXResultToString(NVSDK_NGX_Result result) {
    auto * wstr = GetNGXResultAsString(result);
    if (!wstr) return std::format("0x{:08X}", static_cast<uint32_t>(result));
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str.data(), len, nullptr, nullptr);
    if (!str.empty() && str.back() == '\0') str.pop_back();
    return str;
}

static void NGXLogCallback(const char * message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    MI_LOG(MIInfraLogType::kInfo, "[NGX] {}", message);
}

void NGXContext::GetRequiredVulkanExtensions(std::vector<const char*>& out_instance_extensions,
                                                      std::vector<const char*>& out_device_extensions) {
    unsigned int inst_count = 0;
    const char ** inst_exts = nullptr;
    unsigned int dev_count = 0;
    const char ** dev_exts = nullptr;

    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_RequiredExtensions(
        &inst_count, &inst_exts,
        &dev_count, &dev_exts
    );

    if (NVSDK_NGX_SUCCEED(result)) {
        for (unsigned int i = 0; i < inst_count; ++i) {
            out_instance_extensions.push_back(inst_exts[i]);
        }
        for (unsigned int i = 0; i < dev_count; ++i) {
            out_device_extensions.push_back(dev_exts[i]);
        }
    } else {
        MI_LOG(MIInfraLogType::kWarning, "NGX: Failed to query required Vulkan extensions.");
    }
}

bool NGXContext::ProbeAvailability() {
    auto * handles = static_cast<const VulkanRHIHandles *>(RHI::Get().GetUnderlyingGraphicsAPIHandles());
    if (!handles) {
        MI_LOG(MIInfraLogType::kWarning, "DLSS: ProbeAvailability failed — no Vulkan handles.");
        return false;
    }

    NVSDK_NGX_FeatureDiscoveryInfo discovery_info {};
    discovery_info.SDKVersion = NVSDK_NGX_Version_API;
    discovery_info.FeatureID  = NVSDK_NGX_Feature_RayReconstruction;
    discovery_info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery_info.Identifier.v.ProjectDesc.ProjectId   = kDLSSProjectId;
    discovery_info.Identifier.v.ProjectDesc.EngineType  = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    discovery_info.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    discovery_info.ApplicationDataPath = L".";
    discovery_info.FeatureInfo = nullptr;

    NVSDK_NGX_FeatureRequirement requirement {};
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureRequirements(
        handles->instance, handles->physical_device, &discovery_info, &requirement);

    if (NVSDK_NGX_FAILED(result)) {
        MI_LOG(MIInfraLogType::kWarning, "DLSS: NVSDK_NGX_VULKAN_GetFeatureRequirements failed — {}.", NGXResultToString(result));
        return false;
    }
    if (requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        MI_LOG(MIInfraLogType::kWarning, "DLSS: Ray Reconstruction not supported — FeatureSupported = {}.", (uint32_t)requirement.FeatureSupported);
        return false;
    }
    return true;
}

NGXContext::~NGXContext() {
    Shutdown();
}

bool NGXContext::Initialize() {
    auto * handles = static_cast<const VulkanRHIHandles *>(RHI::Get().GetUnderlyingGraphicsAPIHandles());
    if (!handles) {
        MI_LOG(MIInfraLogType::kError, "DLSS: Failed to get Vulkan handles.");
        return false;
    }

    std::promise<bool> init_promise;
    auto init_future = init_promise.get_future();

    auto rhi_fut = EnqueueRHIThreadTask(
        [&, handles]() {
            NVSDK_NGX_FeatureCommonInfo feature_info {};
            NVSDK_NGX_LoggingInfo log_info {};
            log_info.LoggingCallback = NGXLogCallback;
            log_info.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
            feature_info.LoggingInfo = log_info;

            NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
                kDLSSProjectId,
                NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                "1.0",
                L".",
                handles->instance,
                handles->physical_device,
                handles->device,
                vkGetInstanceProcAddr,
                vkGetDeviceProcAddr,
                &feature_info
            );
            if (NVSDK_NGX_FAILED(result)) {
                MI_LOG(MIInfraLogType::kError, "DLSS: NGX initialization failed — {}.", NGXResultToString(result));
                init_promise.set_value(false);
                return;
            }
            init_promise.set_value(true);
        }
    );

    rhi_fut.wait();
    bool success = init_future.get();
    if (!success) return false;

    is_initialized_ = true;
    MI_INFO("DLSS: Initialized on vulkan.");
    return true;
}

void NGXContext::Shutdown() {
    if (!is_initialized_) return;

    auto * handles = static_cast<const VulkanRHIHandles *>(RHI::Get().GetUnderlyingGraphicsAPIHandles());
    if (handles) {
        NVSDK_NGX_VULKAN_Shutdown1(handles->device);
    }

    is_initialized_ = false;
}

MI_NAMESPACE_END
