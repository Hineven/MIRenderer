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
#include "core/pixel_format.h"
#include "core/refcounted.h"
#include "rhi/rhi_fwd.h"
#include "renderer/mi_renderer_fwd.h"

MI_NAMESPACE_BEGIN

class Texture : public RefCounted<>, public NonMovable {
public:
    void InitializeFromBinary (std::span<uint8_t> data);

    FORCEINLINE uint32_t GetWidth () const { return width_; }
    FORCEINLINE uint32_t GetHeight () const { return height_; }
    FORCEINLINE PixelFormatType GetFormat () const { return format_; }

    FORCEINLINE BindlessRendererTexture * GetDeviceBindlessTexture () const {
        return device_texture_bindless_.Raw();
    }
    FORCEINLINE RHITexture * GetDeviceTexture () const {
        return device_texture_.Raw();
    }

    bool IsBindless () const {
        return device_texture_bindless_ != nullptr;
    }

    FORCEINLINE bool IsDirty () const {
        return dirty_;
    }

    FORCEINLINE void SetName (const std::string & name) {
        name_ = name;
    }

    void CreateOnDevice ();
    // You need to manually synchronize on the command queue after calling this for the device texture
    // to become available.
    void CreateOnDevice_Async (RHICommandQueueGraphics & queue);

    void ConvertToBindless (RenderResourceAllocator * alloc);
    void ReleaseBindlessSlot ();

    FORCEINLINE static TRef<Texture> Create (PixelFormatType format, uint32_t width, uint32_t height) {
        return TRef(new Texture(format, width, height));
    }

protected:
    Texture(PixelFormatType format, uint32_t width, uint32_t height);

    std::string name_;

    uint32_t width_ {}, height_ {};
    PixelFormatType format_ {PixelFormatType::kUnknown};
    std::vector<uint8_t> data_;

    bool dirty_ {};

    // Device handle of the texture ( if created )
    TRef<RHITexture> device_texture_;

    // Bindless texture handle, if the texture is uploaded to device and converted to bindless
    // (assigned to a bindless slot).
    TRef<BindlessRendererTexture> device_texture_bindless_;
};

MI_NAMESPACE_END

#endif //MI_TEXTURE_H
