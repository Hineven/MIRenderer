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
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "core/base.h"
#include "core/infra.h"
#include "core/pixel_format.h"
#include "core/refcounted.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_types.h"
#include "renderer/mi_renderer_fwd.h"
#include "rhi/rhi_bindlesskeeper.h"

MI_NAMESPACE_BEGIN

enum class TextureSampleMode {
    kNearestClamp,
    kNearestWrap,
    kLinearClamp,
    kLinearWrap,
};

// An ususally static 2D (array) texture resource.
//
// CPU binary mirror: the constructor ALWAYS allocates `data_` zero-filled
// (width * height * bpp * layers bytes) and the mirror is always uploaded to
// the device on UpdateOnDevice. A freshly-constructed texture therefore has a
// valid (all-zero) CPU mirror and no GPU resource until UpdateOnDevice. For
// GPU-only textures (e.g. the env cubemap, filled by a render pass) the initial
// zero upload is harmless — it is overwritten by the render pass — and the
// convenience of always having a populated mirror outweighs the one-time cost.
// Procedural generators write via GetBinaryMutable(); loaders use
// CreateFromBinary() / InitializeFromBinary().
class Texture : public RefCounted<>, public NonMovable {
public:
    // ---- CPU binary mirror access ----
    // Whole-texture read-only view (always populated; zero-filled until filled
    // by InitializeFromBinary / SetBinaryForLayer / GetBinaryMutable).
    void GetBinary (std::vector<uint8_t> & data) const {
        data = data_;
    }
    const std::vector<uint8_t> & GetBinary () const {
        return data_;
    }
    // Writable view of the CPU mirror, for procedural generation. Marks the
    // texture dirty so the next UpdateOnDevice re-uploads it. NOT move
    // semantics: the returned reference aliases the internal storage; keep the
    // texture alive while you write through it.
    std::vector<uint8_t> & GetBinaryMutable () {
        dirty_ = true;
        return data_;
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
    FORCEINLINE uint32_t GetMipLevels () const { return mip_levels_; }
    FORCEINLINE uint32_t GetArrayLayers () const { return layers_; }
    FORCEINLINE RHITextureType GetType () const { return type_; }
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

    // CPU side texture access operation.
    glm::vec4 Load (uint32_t x, uint32_t y, uint32_t layer = 0) const;
    // CPU side texture access operation.
    glm::vec4 Sample (TextureSampleMode mode, glm::vec2 uv, uint32_t layer = 0) const;

    // shortcut.
    FORCEINLINE uint32_t GetBindlessIndex (bool validation = true) {
        uint32_t index = device_bindless_slot ? device_bindless_slot->GetSlot() : UINT32_MAX;
        if (validation) {
            assert(index != UINT32_MAX && "Texture is not bindless.");
        }
        return index;
    }

    void UpdateOnDevice ();
    // You need to manually synchronize on the command queue after calling this for the device texture
    // to become available.
    void UpdateOnDevice_Async (RHICommandQueueGraphics & queue);

    // Convert the texture to bindless. If update_slot_immediately is true,
    // the bindless slot will be allocated and updated on device immediately.
    void ConvertToBindless (bool update_slot_immediately = true);
    void ReleaseBindlessSlot ();

    // Create a texture with an empty (zero-filled) CPU mirror. Use GetBinaryMutable()
    // to fill it procedurally, or use CreateFromBinary to supply data at once.
    FORCEINLINE static TRef<Texture> Create (PixelFormatType format, uint32_t width, uint32_t height, uint32_t mip_levels = 1, uint32_t array_layers = 1) {
        return TRef(new Texture(RHITextureType::k2D, format, width, height, mip_levels, array_layers));
    }
    FORCEINLINE static TRef<Texture> Create (RHITextureType type, PixelFormatType format, uint32_t width, uint32_t height, uint32_t mip_levels = 1, uint32_t array_layers = 1) {
        return TRef(new Texture(type, format, width, height, mip_levels, array_layers));
    }
    // Create a texture and immediately fill its CPU mirror from `data` (whole
    // texture: width * height * bpp * layers bytes). The texture is dirty after
    // this. Constructor-style convenience over Create(...) + GetBinaryMutable()
    // + memcpy.
    static TRef<Texture> CreateFromBinary (PixelFormatType format, uint32_t width, uint32_t height,
                                           std::span<uint8_t> data, uint32_t mip_levels = 1, uint32_t array_layers = 1) {
        auto tex = Create(RHITextureType::k2D, format, width, height, mip_levels, array_layers);
        tex->InitializeFromBinary(data);
        return tex;
    }
    static TRef<Texture> CreateFromBinary (RHITextureType type, PixelFormatType format, uint32_t width, uint32_t height,
                                           std::span<uint8_t> data, uint32_t mip_levels = 1, uint32_t array_layers = 1) {
        auto tex = Create(type, format, width, height, mip_levels, array_layers);
        tex->InitializeFromBinary(data);
        return tex;
    }
    // Fill the CPU mirror from `data` (whole texture: width * height * bpp *
    // layers bytes). Kept as the lower-level fill (CreateFromBinary delegates
    // here); prefer CreateFromBinary at call sites.
    void InitializeFromBinary (std::span<uint8_t> data);

    FORCEINLINE const std::string & GetName () const {
        return name_;
    }

protected:
    Texture(RHITextureType type, PixelFormatType format, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t layers);

    std::string name_;

    RHITextureType type_ {};

    uint32_t width_ {}, height_ {};
    uint32_t mip_levels_ {}, layers_ {};

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
