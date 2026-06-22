/*
 * Created: 2025/11/29
 * Author: Exploring Air Joe
 * See LICENSE for licensing.
 */

#include "renderer/mi_volume_texture.h"

#include "rdg/rdg_helper.h"
#include "rhi/rhi.h"
#include "rhi/rhi_bindless.h"
#include "rhi/rhi_bindlesskeeper.h"
#include "rhi/rhi_texture.h"

MI_NAMESPACE_BEGIN

VolumeTexture::VolumeTexture(RHITextureType type, PixelFormatType format, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip_levels, uint32_t layers)
    : type_(type), format_(format), width_(width), height_(height), depth_(depth), mip_levels_(mip_levels), layers_(layers), dirty_(true) {
    // Always allocate the CPU mirror zero-filled; the mirror is always uploaded
    // on UpdateOnDevice.
    size_t needed_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) *
                         static_cast<size_t>(depth_) *
                         static_cast<size_t>(GetPixelFormatBytesPerPixel(format_)) *
                         static_cast<size_t>(layers_);
    data_.assign(needed_size, 0);
}

void VolumeTexture::UpdateOnDevice() {
    if (dirty_ || !device_texture_) {
        auto& queue = RHI::Get().GetGraphicsCommandQueue();
        UpdateOnDevice_Async(queue);
        RHI::Get().GetGraphicsCommandQueue().WaitForIdle("VolumeTexture::UpdateOnDevice " + GetName());
    }
}

void VolumeTexture::UpdateOnDevice_Async(RHICommandQueueGraphics& queue) {
    if (!dirty_ && device_texture_) {
        return;
    }

    RHITextureDesc desc;
    desc.type = type_;
    desc.dimensions = { width_, height_, depth_ }; // 深度填入 z 分量
    desc.mip_levels = mip_levels_;
    desc.array_layers = layers_;
    desc.format = format_;
    desc.usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst | extra_device_usage_;

    device_texture_ = RHI::Get().CreateTexture(desc);

    // Always upload the CPU mirror (zero-filled until filled by a loader).
    Helpers::Upload_Async(queue, device_texture_.Raw(), data_.data(), data_.size(),
        RHITextureLayoutType::kShaderReadOnlyOptimal, RHIGPUAccessFlagBits::kShaderRead);

    dirty_ = false;
}

void VolumeTexture::ConvertToBindless(bool update_immediately) {
    if (IsBindless()) return;

    if (!device_texture_) {
        UpdateOnDevice();
    }

    device_bindless_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>(RHIBindlessResourceType::kVolumeSRV);
    device_bindless_slot->Set(device_texture_.Raw());

    if (update_immediately) {
        device_bindless_slot->Commit();
    }
}

void VolumeTexture::ReleaseBindlessSlot() {
    device_bindless_slot.SafeRelease();
}

MI_NAMESPACE_END