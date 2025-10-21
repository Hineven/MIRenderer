/*
 * Created: 2025/4/26
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_texture.h"

#include "rdg/rdg_helper.h"
#include "renderer/mi_resource_allocator.h"
#include "rhi/rhi.h"
#include "rhi/rhi_bindless.h"
#include "rhi/rhi_bindlesskeeper.h"
#include "rhi/rhi_texture.h"

MI_NAMESPACE_BEGIN

Texture::Texture(RHITextureType type, PixelFormatType format, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t layers)
    : type_(type),
    width_(width),
    height_(height),
    mip_levels_(mip_levels),
    layers_(layers),
    format_(format),
    dirty_(true) {
}

void Texture::InitializeFromBinary(std::span<uint8_t> data)
{
    size_t needed_size = width_ * height_ * GetPixelFormatBytesPerPixel(format_);
    if (data.size() != needed_size) {
        MI_WARN("Texture::InitializeFromBinary(): Incorrect data size. Expected{}, got {}.", needed_size, data.size());
        return;
    }

    data_.resize(needed_size);
    data_.shrink_to_fit();
    std::memcpy(data_.data(), data.data(), needed_size);
    
    dirty_ = true;
}

void Texture::UpdateOnDevice()
{
    if (dirty_ || !device_texture_) {
        auto & queue = RHI::Get().GetGraphicsCommandQueue();
        UpdateOnDevice_Async(queue);
        RHI::Get().GetGraphicsCommandQueue().WaitForIdle("Texture::UpdateOnDevice " + GetName());
    }
}

void Texture::UpdateOnDevice_Async(RHICommandQueueGraphics& queue)
{
    if (!dirty_ && device_texture_) {
        return;
    }
    
    RHITextureDesc desc;
    desc.type = type_;
    desc.array_layers = layers_;
    if (desc.type == RHITextureType::kCube) {
        assert(desc.array_layers == 6);
    }
    desc.dimensions = {width_, height_, 1};
    desc.mip_levels = mip_levels_;
    desc.array_layers = layers_;
    desc.format = format_;
    desc.usage = RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst | extra_device_usage_;

    device_texture_ = RHI::Get().CreateTexture(desc);

    if (!data_.empty()) {
        Helpers::Upload_Async(queue, device_texture_.Raw(), data_.data(), data_.size(),
            RHITextureLayoutType::kShaderReadOnlyOptimal, RHIGPUAccessFlagBits::kShaderRead);
    }
    
    dirty_ = false;
}

void Texture::ConvertToBindless(bool update_immediately)
{
    if (IsBindless()) {
        return;
    }

    // Make sure the texture is uploaded to device
    if (!device_texture_) {
        UpdateOnDevice();
    }

    // Allocate a bindless slot
    device_bindless_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>();
    device_bindless_slot->Set(device_texture_.Raw());
    if (update_immediately) {
        device_bindless_slot->Commit();
    }

}

void Texture::ReleaseBindlessSlot() {
    device_bindless_slot.SafeRelease();
}

MI_NAMESPACE_END
