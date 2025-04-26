/*
 * Created: 2025/4/26
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_TEXTURE_H
#define MI_TEXTURE_H

#include <memory>
#include <span>

#include "core/base.h"
#include "core/pixel_format.h"
#include "core/refcounted.h"
#include "rhi/rhi_fwd.h"
#include "renderer/mi_renderer_fwd.h"

MI_NAMESPACE_BEGIN

class Texture : public RefCounted<>, public NonMovable {
public:
    Texture(PixelFormatType format, uint32_t width, uint32_t height);
    void InitializeFromBinary (std::span<uint8_t> data);

    FORCEINLINE uint32_t GetWidth () const { return width_; }
    FORCEINLINE uint32_t GetHeight () const { return height_; }
    FORCEINLINE PixelFormatType GetFormat () const { return format_; }

    FORCEINLINE RHITexture * GetDeviceTexture () const {
        return device_texture_.Raw();
    }
    FORCEINLINE bool IsDirty () const {
        return dirty_;
    }

    void CreateOnDevice (RenderResourceAllocator * alloc);
    // You need to manually synchronize on the command queue after calling this for the device texture
    // to become available.
    void CreateOnDevice_Async (RenderResourceAllocator * alloc, RHICommandQueueGraphics & queue);

protected:
    uint32_t width_ {}, height_ {};
    PixelFormatType format_ {PixelFormatType::kUnknown};
    std::unique_ptr<uint8_t[]> data_;

    bool dirty_ {};
    TRef<RHITexture> device_texture_;
};

MI_NAMESPACE_END

#endif //MI_TEXTURE_H
