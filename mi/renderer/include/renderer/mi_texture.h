/*
 * Created: 2025/4/26
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_TEXTURE_H
#define MI_TEXTURE_H

#include <memory>
#include <span>
#include <vector>

#include "core/base.h"
#include "core/infra.h"
#include "core/pixel_format.h"
#include "core/refcounted.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_types.h"
#include "renderer/mi_renderer_fwd.h"
#include "rhi/rhi_bindlesskeeper.h"

MI_NAMESPACE_BEGIN
    class Texture : public RefCounted<>, public NonMovable {
public:
    void InitializeFromBinary (std::span<uint8_t> data);
    void GetBinary (std::vector<uint8_t> & data) const {
        data = data_;
    }
    void GetBinaryForLayer (uint32_t layer, std::vector<uint8_t> & data) const {
        size_t needed_size = width_ * height_ * GetPixelFormatBytesPerPixel(format_);
        data.resize(needed_size);
        data.shrink_to_fit();
        std::memcpy(data.data(), data_.data() + layer * needed_size, needed_size);
    }
    void SetBinaryForLayer (uint32_t layer, std::span<uint8_t> data) {
        size_t needed_size = width_ * height_ * GetPixelFormatBytesPerPixel(format_);
        if (data.size() != needed_size) {
            MI_WARN("Texture::SetBinaryForLayer(): Incorrect data size. Expected {}, got {}.", needed_size, data.size());
            return;
        }
        std::memcpy(data_.data() + layer * needed_size, data.data(), needed_size);
        dirty_ = true;
    }

    FORCEINLINE uint32_t GetWidth () const { return width_; }
    FORCEINLINE uint32_t GetHeight () const { return height_; }
    FORCEINLINE PixelFormatType GetFormat () const { return format_; }

    FORCEINLINE RHIBindlessSlotKeeper<RHITexture> * GetBindlessSlot () const {
        return device_bindless_slot.Raw();
    }
    FORCEINLINE RHITexture * GetDeviceTexture () const {
        return device_texture_.Raw();
    }

    FORCEINLINE bool IsBindless () const {
        return device_bindless_slot != nullptr;
    }

    FORCEINLINE bool IsDirty () const {
        return dirty_;
    }

    FORCEINLINE void SetName (const std::string & name) {
        name_ = name;
    }

    FORCEINLINE void AddDeviceUsage (RHITextureUsageFlags usage) {
        extra_device_usage_ = extra_device_usage_ | usage;
    }

    // shortcut.
    FORCEINLINE uint32_t GetBindlessIndex (bool validation = true) {
        uint32_t index = device_bindless_slot ? device_bindless_slot->GetSlot() : UINT32_MAX;
        if (validation) {
            assert(index != UINT32_MAX);
        }
        return index;
    }

    void UpdateOnDevice ();
    // You need to manually synchronize on the command queue after calling this for the device texture
    // to become available.
    void UpdateOnDevice_Async (RHICommandQueueGraphics & queue);

    void ConvertToBindless (bool update_slot_immediately = true);
    void ReleaseBindlessSlot ();

    FORCEINLINE static TRef<Texture> Create (PixelFormatType format, uint32_t width, uint32_t height, uint32_t array_layers = 1) {
        return TRef(new Texture(RHITextureType::k2D, format, width, height, array_layers));
    }
    FORCEINLINE static TRef<Texture> Create (RHITextureType type, PixelFormatType format, uint32_t width, uint32_t height, uint32_t array_layers = 1) {
        return TRef(new Texture(type, format, width, height, array_layers));
    }

protected:
    Texture(RHITextureType type, PixelFormatType format, uint32_t width, uint32_t height, uint32_t layers);

    std::string name_;

    RHITextureType type_ {};
    uint32_t width_ {}, height_ {}, layers_ {};
    PixelFormatType format_ {PixelFormatType::kUnknown};
    std::vector<uint8_t> data_;

    RHITextureUsageFlags extra_device_usage_ {};

    bool dirty_ {};

    // Device handle of the texture (if created)
    TRef<RHITexture> device_texture_;

    // Bindless slot (if created)
    TRef<RHIBindlessSlotKeeper<RHITexture>> device_bindless_slot;
};

MI_NAMESPACE_END

#endif //MI_TEXTURE_H
