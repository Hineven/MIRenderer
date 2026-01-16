/*
 * Created: 2025/11/29
 * Author: Exploring Air Joe
 * See LICENSE for licensing.
 */

#ifndef MI_VOLUME_TEXTURE_H
#define MI_VOLUME_TEXTURE_H

#include <vector>
#include <span>
#include <memory>

#include "renderer/mi_renderer_fwd.h"
#include "core/base.h"
#include "core/infra.h"
#include "core/pixel_format.h"
#include "core/refcounted.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_types.h"
#include "rhi/rhi_bindlesskeeper.h"

MI_NAMESPACE_BEGIN

// 专门用于存储体渲染数据的 3D 纹理资源
class VolumeTexture : public RefCounted<>, public NonMovable {
public:
    // 创建Volume纹理
    FORCEINLINE static TRef<VolumeTexture> Create(PixelFormatType format,
        uint32_t width, uint32_t height, uint32_t depth,
        uint32_t mip_levels = 1, uint32_t array_layers = 1) {
        return TRef(new VolumeTexture(RHITextureType::k3D, format, width, height, depth, mip_levels, array_layers));
    }
    FORCEINLINE static TRef<VolumeTexture> Create(RHITextureType type, PixelFormatType format,
        uint32_t width, uint32_t height, uint32_t depth,
        uint32_t mip_levels = 1, uint32_t array_layers = 1) {
        return TRef(new VolumeTexture(type, format, width, height, depth, mip_levels, array_layers));
    }

    // 纹理命名相关
    FORCEINLINE const std::string & GetName () const { return name_; }
    FORCEINLINE void SetName(const std::string& name) { name_ = name; }

    // 基础属性访问器
    FORCEINLINE RHITextureType GetType () const { return type_; }
    FORCEINLINE uint32_t GetWidth () const { return width_; }
    FORCEINLINE uint32_t GetHeight () const { return height_; }
    FORCEINLINE uint32_t GetDepth () const { return depth_; }
    FORCEINLINE uint32_t GetMipLevels () const { return mip_levels_; }
    FORCEINLINE uint32_t GetArrayLayers () const { return layers_; }
    FORCEINLINE PixelFormatType GetFormat () const { return format_; }

    // 数据存取
    void GetBinary (std::vector<uint8_t> & data) const { data = data_; }
    const std::vector<uint8_t> & GetBinary () { return data_; }
    void GetBinaryForLayer (uint32_t layer, std::vector<uint8_t> & data) const {
        size_t needed_size = width_ * height_ * depth_ * GetPixelFormatBytesPerPixel(format_);
        data.resize(needed_size);
        data.shrink_to_fit();
        std::memcpy(data.data(), data_.data() + layer * needed_size, needed_size);
    }

    void SetBinary (std::span<uint8_t> data) {
        size_t needed_size = width_ * height_ * depth_ * GetPixelFormatBytesPerPixel(format_);
        if (data.size() != needed_size) {
            MI_WARN("Texture::InitializeFromBinary(): Incorrect data size. Expected{}, got {}.", needed_size, data.size());
            return;
        }
        data_.resize(needed_size);
        data_.shrink_to_fit();
        std::memcpy(data_.data(), data.data(), needed_size);
        dirty_ = true;
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

    // 添加资源使用方式
    FORCEINLINE void AddDeviceUsage (RHITextureUsageFlags usage) {
        extra_device_usage_ = extra_device_usage_ | usage;
    }

    // dirty位访问
    FORCEINLINE bool IsDirty() const { return dirty_; }

    // RHI / GPU 资源访存
    FORCEINLINE RHITexture* GetDeviceTexture() const { return device_texture_.Raw(); }
    void UpdateOnDevice();
    void UpdateOnDevice_Async(RHICommandQueueGraphics& queue);

    // Bindless 支持
    FORCEINLINE bool IsBindless() const { return device_bindless_slot != nullptr; }
    FORCEINLINE RHIBindlessSlotKeeper<RHITexture>* GetBindlessSlot() const { return device_bindless_slot.Raw(); }
    FORCEINLINE uint32_t GetBindlessIndex(bool validation = true) {
        uint32_t index = device_bindless_slot ? device_bindless_slot->GetSlot() : UINT32_MAX;
        if (validation) {
            mi_assert(index != UINT32_MAX, "VolumeTexture is not bindless.");
        }
        return index;
    }
    void ConvertToBindless(bool update_slot_immediately = true);
    void ReleaseBindlessSlot();

protected:
    VolumeTexture(RHITextureType type, PixelFormatType format,
        uint32_t width, uint32_t height, uint32_t depth,
        uint32_t mip_levels, uint32_t layers);

    std::string name_;

    RHITextureType type_ {};
    uint32_t width_{}, height_{}, depth_{};
    uint32_t mip_levels_ {}, layers_ {};
    PixelFormatType format_{ PixelFormatType::kUnknown };

    std::vector<uint8_t> data_;

    RHITextureUsageFlags extra_device_usage_ {};

    bool dirty_{};

    // Device 资源
    TRef<RHITexture> device_texture_;
    TRef<RHIBindlessSlotKeeper<RHITexture>> device_bindless_slot;
};

MI_NAMESPACE_END

#endif //MI_VOLUME_TEXTURE_H