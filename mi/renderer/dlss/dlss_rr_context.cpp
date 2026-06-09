/*
 * Created: 2026/05/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <windows.h>

#include "dlss_rr_context.h"
#include "ngx_context.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_pool.h"
#include "rhi/rhi.h"
#include "rhi/rhi_cmd_exec.h"
#include "rhi/rhi_thread.h"
#include "rhi/vk/vk_export.h"

// NGX SDK headers
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

MI_NAMESPACE_BEGIN

// Extract Vulkan resource info from an RDGTexture on the Render Thread.
static DLSSRRResourceSnapshot::Res SnapshotRes(RDGTexture * tex) {
    DLSSRRResourceSnapshot::Res r {};
    if (!tex) return r;
    auto * rhi_tex = tex->GetRHI();
    if (!rhi_tex) return r;
    VulkanTextureNativeInfo info {};
    if (!RHIGetVulkanTextureInfo(rhi_tex, &info)) return r;
    r.image_view = info.image_view;
    r.image      = info.image;
    r.format     = static_cast<VkFormat>(info.format);
    r.width      = info.width;
    r.height     = info.height;
    return r;
}

// Reconstruct NVSDK_NGX_Resource_VK from a pre-extracted snapshot.
static NVSDK_NGX_Resource_VK ToNGXResource(const DLSSRRResourceSnapshot::Res & r, bool read_write) {
    NVSDK_NGX_Resource_VK res {};
    if (!r.image_view) return res;
    res.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    res.ReadWrite = read_write;
    res.Resource.ImageViewInfo.ImageView = static_cast<VkImageView>(r.image_view);
    res.Resource.ImageViewInfo.Image     = static_cast<VkImage>(r.image);
    res.Resource.ImageViewInfo.SubresourceRange = vk::ImageSubresourceRange{
        vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    res.Resource.ImageViewInfo.Format = static_cast<VkFormat>(r.format);
    res.Resource.ImageViewInfo.Width  = r.width;
    res.Resource.ImageViewInfo.Height = r.height;
    return res;
}

static std::string NGXResultToString(NVSDK_NGX_Result result) {
    auto * wstr = GetNGXResultAsString(result);
    if (!wstr) return std::format("0x{:08X}", static_cast<uint32_t>(result));
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str.data(), len, nullptr, nullptr);
    if (!str.empty() && str.back() == '\0') str.pop_back();
    return str;
}

DLSSRRContext::DLSSRRContext() = default;

DLSSRRContext::~DLSSRRContext() {
    Shutdown();
}

void DLSSRRContext::CreateAuxiliaryTextures(uint32_t width, uint32_t height) {
    auto usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess;
    auto usage_with_transfer_src = usage | RHITextureUsageFlagBits::kTransferSrc;

    pt_depth_ = RDGTexture::Create2D(width, height, PixelFormatType::kR32_FLOAT, usage);
    pt_depth_->SetName("PT Depth (DLSS)");
    pt_depth_->SetExport();

    pt_normal_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8G8B8A8_UNORM, usage);
    pt_normal_->SetName("PT Normal (DLSS)");
    pt_normal_->SetExport();

    pt_motion_vector_ = RDGTexture::Create2D(width, height, PixelFormatType::kR16G16_FLOAT, usage);
    pt_motion_vector_->SetName("PT Motion Vector (DLSS)");
    pt_motion_vector_->SetExport();

    pt_albedo_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8G8B8A8_UNORM, usage);
    pt_albedo_->SetName("PT Albedo (DLSS)");
    pt_albedo_->SetExport();

    pt_specular_albedo_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8G8B8A8_UNORM, usage);
    pt_specular_albedo_->SetName("PT SpecularAlbedo (DLSS)");
    pt_specular_albedo_->SetExport();

    pt_roughness_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8_UNORM, usage);
    pt_roughness_->SetName("PT Roughness (DLSS)");
    pt_roughness_->SetExport();

    pt_alpha_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8_UNORM, usage);
    pt_alpha_->SetName("PT Alpha (DLSS)");
    pt_alpha_->SetExport();

    dlss_output_ = RDGTexture::Create2D(width, height, PixelFormatType::kR16G16B16A16_FLOAT, usage_with_transfer_src);
    dlss_output_->SetName("DLSS Output");
    dlss_output_->SetExport();
}

bool DLSSRRContext::Initialize(NGXContext * ngx, uint32_t width, uint32_t height) {
    if (!ngx || !ngx->IsInitialized()) {
        MI_LOG(MIInfraLogType::kError, "DLSS-RR: NGX context is not initialized.");
        return false;
    }
    return SetResolution(width, height);
}

void DLSSRRContext::Shutdown() {
    ReleaseResolution();
}

void DLSSRRContext::ReleaseResolution() {
    if (!is_available_) return;

    if (ngx_feature_) {
        NVSDK_NGX_VULKAN_ReleaseFeature(static_cast<NVSDK_NGX_Handle *>(ngx_feature_));
        ngx_feature_ = nullptr;
    }

    if (ngx_parameters_) {
        NVSDK_NGX_VULKAN_DestroyParameters(static_cast<NVSDK_NGX_Parameter *>(ngx_parameters_));
        ngx_parameters_ = nullptr;
    }

    dlss_output_        = nullptr;
    pt_depth_           = nullptr;
    pt_normal_          = nullptr;
    pt_motion_vector_   = nullptr;
    pt_albedo_          = nullptr;
    pt_specular_albedo_ = nullptr;
    pt_roughness_       = nullptr;
    pt_alpha_           = nullptr;

    is_available_   = false;
    render_width_   = 0;
    render_height_  = 0;
}

bool DLSSRRContext::SetResolution(uint32_t width, uint32_t height) {
    if (width == render_width_ && height == render_height_ && is_available_) return true;

    if (is_available_) {
        ReleaseResolution();
    }

    auto * handles = static_cast<const VulkanRHIHandles *>(RHI::Get().GetUnderlyingGraphicsAPIHandles());
    if (!handles) {
        MI_LOG(MIInfraLogType::kError, "DLSS-RR: Failed to get Vulkan handles.");
        return false;
    }

    std::promise<bool> init_promise;
    auto init_future = init_promise.get_future();

    auto rhi_fut = EnqueueRHIThreadTask(
        [&, handles, width, height]() {
            NVSDK_NGX_Parameter * params = nullptr;
            auto result = NVSDK_NGX_VULKAN_AllocateParameters(&params);
            if (NVSDK_NGX_FAILED(result)) {
                MI_LOG(MIInfraLogType::kError, "DLSS-RR: Failed to allocate NGX parameters.");
                init_promise.set_value(false);
                return;
            }

            // --- Transient command buffer approach (matching NVIDIA sample pattern) ---
            // Create a dedicated transient cmd buffer for CreateFeature,
            // submit it, wait for GPU completion, then destroy the pool.
            // This ensures the DLL never sees a recycled/shared cmd buffer.
            vk::Device dev = handles->device;
            vk::CommandPool transient_pool = dev.createCommandPool(
                vk::CommandPoolCreateInfo{}
                    .setQueueFamilyIndex(handles->graphics_queue_family_index)
                    .setFlags(vk::CommandPoolCreateFlagBits::eTransient)
            );
            vk::CommandBuffer transient_cmd = dev.allocateCommandBuffers(
                vk::CommandBufferAllocateInfo{}
                    .setCommandPool(transient_pool)
                    .setLevel(vk::CommandBufferLevel::ePrimary)
                    .setCommandBufferCount(1)
            )[0];
            transient_cmd.begin(vk::CommandBufferBeginInfo{}
                .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

            VkCommandBuffer vk_cmd = static_cast<VkCommandBuffer>(transient_cmd);

            NVSDK_NGX_DLSSD_Create_Params create_params {};
            create_params.InDenoiseMode      = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
            create_params.InRoughnessMode    = NVSDK_NGX_DLSS_Roughness_Mode_Unpacked;
            create_params.InUseHWDepth       = NVSDK_NGX_DLSS_Depth_Type_Linear;
            create_params.InWidth            = width;
            create_params.InHeight           = height;
            create_params.InTargetWidth      = width;
            create_params.InTargetHeight     = height;
            create_params.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
            create_params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
            create_params.InEnableOutputSubrects = false;

            NVSDK_NGX_Handle * handle = nullptr;
            NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSSD_EXT1(
                handles->device,
                vk_cmd,
                1, 1,
                &handle,
                params,
                &create_params
            );

            if (NVSDK_NGX_SUCCEED(r)) {
                // End recording and submit the transient cmd buffer
                transient_cmd.end();
                vk::SubmitInfo submit_info {};
                submit_info.setCommandBufferCount(1);
                submit_info.setPCommandBuffers(&transient_cmd);
                handles->graphics_queue.submit(submit_info, nullptr);
                // Wait for GPU to finish processing CreateFeature
                handles->graphics_queue.waitIdle();

                ngx_feature_    = handle;
                ngx_parameters_ = params;
                init_promise.set_value(true);
            } else {
                MI_WARN("DLSS-RR: Ray Reconstruction feature creation failed: {}", NGXResultToString(r));
                NVSDK_NGX_VULKAN_DestroyParameters(params);
                init_promise.set_value(false);
            }

            // Destroy the transient pool (frees the cmd buffer)
            dev.destroyCommandPool(transient_pool);
        });

    rhi_fut.wait();
    bool success = init_future.get();
    if (!success) return false;

    // Ensure the RHI thread has finished all pending tasks before proceeding.
    // Note: CreateFeature's transient cmd buffer was already submitted and waited on
    // inside the lambda above. This WaitForIdle drains any remaining RHI thread work.
    RHI::Get().WaitForIdle();

    render_width_   = width;
    render_height_  = height;
    CreateAuxiliaryTextures(width, height);
    is_available_   = true;
    MI_LOG(MIInfraLogType::kInfo, "DLSS Ray Reconstruction initialized ({}x{}).", width, height);
    return true;
}

void DLSSRRContext::OnResolutionChanged(uint32_t width, uint32_t height) {
    if (width == render_width_ && height == render_height_) return;
    SetResolution(width, height);
}

DLSSRRResourceSnapshot DLSSRRContext::BuildResourceSnapshot(
    RDGTexture * noisy_radiance,
    RDGTexture * depth,
    RDGTexture * normal,
    RDGTexture * motion_vector,
    RDGTexture * albedo,
    RDGTexture * specular_albedo,
    RDGTexture * roughness,
    RDGTexture * alpha,
    RDGTexture * output)
{
    // This method must be called on the Render Thread where RDG resources are accessible.
    DLSSRRResourceSnapshot snap {};
    snap.color        = SnapshotRes(noisy_radiance);
    snap.depth        = SnapshotRes(depth);
    snap.mv           = SnapshotRes(motion_vector);
    snap.albedo       = SnapshotRes(albedo);
    snap.spec_albedo  = SnapshotRes(specular_albedo);
    snap.normal       = SnapshotRes(normal);
    snap.roughness    = SnapshotRes(roughness);
    snap.alpha        = SnapshotRes(alpha);
    snap.output       = SnapshotRes(output);
    return snap;
}

void DLSSRRContext::Evaluate_RHI(
    const DLSSRRResourceSnapshot & resources,
    float camera_jitter_x,
    float camera_jitter_y,
    bool reset_history,
    const float * world_to_view_matrix,
    const float * view_to_clip_matrix)
{
    if (!is_available_) return;

    auto * feature = static_cast<NVSDK_NGX_Handle *>(ngx_feature_);
    auto * params  = static_cast<NVSDK_NGX_Parameter *>(ngx_parameters_);
    if (!feature || !params) return;

    void * native_cmd = RHI::Get().GetCommandExecutor()->GetCurrentNativeCommandBuffer(RHICommandQueueType::kGraphics);
    VkCommandBuffer vk_cmd = static_cast<VkCommandBuffer>(native_cmd);

    // Reconstruct NGX resource structs from the pre-extracted snapshot (safe on RHI thread).
    NVSDK_NGX_Resource_VK res_color         = ToNGXResource(resources.color, false);
    NVSDK_NGX_Resource_VK res_depth         = ToNGXResource(resources.depth, false);
    NVSDK_NGX_Resource_VK res_mv            = ToNGXResource(resources.mv, false);
    NVSDK_NGX_Resource_VK res_albedo        = ToNGXResource(resources.albedo, false);
    NVSDK_NGX_Resource_VK res_spec_albedo   = ToNGXResource(resources.spec_albedo, false);
    NVSDK_NGX_Resource_VK res_normal        = ToNGXResource(resources.normal, false);
    NVSDK_NGX_Resource_VK res_roughness     = ToNGXResource(resources.roughness, false);
    NVSDK_NGX_Resource_VK res_alpha         = ToNGXResource(resources.alpha, false);
    NVSDK_NGX_Resource_VK res_output        = ToNGXResource(resources.output, true);

    NVSDK_NGX_VK_DLSSD_Eval_Params eval_params {};
    eval_params.pInColor         = res_color.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_color : nullptr;
    eval_params.pInDepth         = res_depth.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_depth : nullptr;
    eval_params.pInMotionVectors = res_mv.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_mv : nullptr;
    eval_params.pInDiffuseAlbedo = res_albedo.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_albedo : nullptr;
    eval_params.pInSpecularAlbedo= res_spec_albedo.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_spec_albedo : nullptr;
    eval_params.pInNormals       = res_normal.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_normal : nullptr;
    eval_params.pInRoughness     = res_roughness.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_roughness : nullptr;
    eval_params.pInOutput        = res_output.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_output : nullptr;
    eval_params.pInAlpha         = res_alpha.Type == NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ? &res_alpha : nullptr;
    eval_params.pInOutputAlpha   = nullptr;

    // Jitter is stored in NDC space in our renderer, DLSS expects pixel space
    eval_params.InJitterOffsetX = camera_jitter_x * render_width_ * 0.5f;
    eval_params.InJitterOffsetY = camera_jitter_y * render_height_ * 0.5f;

    // Motion vectors from the path tracer are in NDC space, computed as (curr - prev).
    // DLSS expects pixel-space MVs. X is negated because our shader convention is (curr - prev)
    // while DLSS expects (prev - curr) for X. Y follows the standard NDC-to-pixel scaling.
    eval_params.InMVScaleX = static_cast<float>(render_width_) * -0.5f;
    eval_params.InMVScaleY = static_cast<float>(render_height_) * 0.5f;

    eval_params.InRenderSubrectDimensions.Width  = render_width_;
    eval_params.InRenderSubrectDimensions.Height = render_height_;

    eval_params.InReset = reset_history ? 1 : 0;
    eval_params.InPreExposure = 1.0f;
    eval_params.InExposureScale = 1.0f;

    eval_params.pInWorldToViewMatrix = const_cast<float*>(world_to_view_matrix);
    eval_params.pInViewToClipMatrix  = const_cast<float*>(view_to_clip_matrix);

    NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSSD_EXT(vk_cmd, feature, params, &eval_params);
    if (NVSDK_NGX_FAILED(result)) {
        MI_LOG(MIInfraLogType::kWarning, "DLSS Ray Reconstruction evaluate failed: {}.", NGXResultToString(result));
    }
}

MI_NAMESPACE_END
