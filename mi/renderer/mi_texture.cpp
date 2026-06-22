/*
 * Created: 2025/4/26
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_texture.h"

#include <cmath>
#include <glm/common.hpp>

#include "rdg/rdg_helper.h"
#include "renderer/mi_resource_allocator.h"
#include "rhi/rhi.h"
#include "rhi/rhi_bindless.h"
#include "rhi/rhi_bindlesskeeper.h"
#include "rhi/rhi_texture.h"

MI_NAMESPACE_BEGIN

namespace {

float DecodeValueWithPixelFormat(PixelFormatType type, const std::byte * data) {
    auto data_type = GetPixelFormatDataType(type);
    auto num_bytes = GetPixelFormatNumBytesPerChannel(type);
    switch (data_type) {
        case PixelFormatDataType::kFLOAT:
            if (num_bytes == 4) {
                float value;
                std::memcpy(&value, data, sizeof(value));
                return value;
            }
            if (num_bytes == 2) {
                uint16_t half;
                std::memcpy(&half, data, sizeof(half));

                uint32_t sign = (half >> 15) & 0x1u;
                uint32_t exp = (half >> 10) & 0x1Fu;
                uint32_t mant = half & 0x3FFu;

                uint32_t bits = 0u;
                if (exp == 0u) {
                    if (mant == 0u) {
                        bits = (sign << 31);
                    } else {
                        int32_t e = -14;
                        while ((mant & 0x0400u) == 0u) {
                            mant <<= 1;
                            --e;
                        }
                        mant &= 0x03FFu;
                        uint32_t exp32 = static_cast<uint32_t>(e + 127);
                        uint32_t mant32 = mant << 13;
                        bits = (sign << 31) | (exp32 << 23) | mant32;
                    }
                } else if (exp == 31u) {
                    if (mant == 0u) {
                        bits = (sign << 31) | 0x7F800000u;
                    } else {
                        bits = 0x7FC00000u;
                    }
                } else {
                    uint32_t exp32 = (exp - 15u + 127u);
                    uint32_t mant32 = mant << 13;
                    bits = (sign << 31) | (exp32 << 23) | mant32;
                }

                float value;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            }
            break;

        case PixelFormatDataType::kUNORM: {
            if (num_bytes == 1) {
                uint8_t value;
                std::memcpy(&value, data, sizeof(value));
                return static_cast<float>(value) / 255.0f;
            }
            if (num_bytes == 2) {
                uint16_t value;
                std::memcpy(&value, data, sizeof(value));
                return static_cast<float>(value) / 65535.0f;
            }
            break;
        }

        case PixelFormatDataType::kSRGB: {
            float encoded = 0.0f;
            if (num_bytes == 1) {
                uint8_t value;
                std::memcpy(&value, data, sizeof(value));
                encoded = static_cast<float>(value) / 255.0f;
            } else if (num_bytes == 2) {
                uint16_t value;
                std::memcpy(&value, data, sizeof(value));
                encoded = static_cast<float>(value) / 65535.0f;
            } else {
                break;
            }

            if (encoded <= 0.04045f) {
                return encoded / 12.92f;
            }
            return std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }

        case PixelFormatDataType::kUINT:
            if (num_bytes == 1) {
                uint8_t value;
                std::memcpy(&value, data, sizeof(value));
                return static_cast<float>(value);
            }
            if (num_bytes == 4) {
                uint32_t value;
                std::memcpy(&value, data, sizeof(value));
                return static_cast<float>(value);
            }
            break;

        default:
            break;
    }
    return 0.0f;
}

int32_t ResolveTexelCoordinate(int32_t coordinate, int32_t extent, bool wrap) {
    if (extent <= 0) {
        return 0;
    }

    if (wrap) {
        int32_t m = coordinate % extent;
        return m < 0 ? m + extent : m;
    }

    return glm::clamp(coordinate, 0, extent - 1);
}

float NormalizeUV(float v) {
    return v - std::floor(v);
}

glm::vec4 LoadTexelFromBinary(
    const std::vector<uint8_t> & data,
    PixelFormatType format,
    uint32_t width,
    uint32_t height,
    uint32_t x,
    uint32_t y,
    uint32_t layer) {
    const uint32_t channels = GetPixelFormatNumChannels(format);
    const uint32_t bytes_per_channel = GetPixelFormatNumBytesPerChannel(format);
    const uint32_t bytes_per_pixel = channels * bytes_per_channel;
    const size_t layer_stride = static_cast<size_t>(width) * static_cast<size_t>(height) * bytes_per_pixel;

    const size_t required_bytes = (static_cast<size_t>(layer) + 1) * layer_stride;
    if (data.size() < required_bytes) {
        return {};
    }

    const size_t pixel_offset = static_cast<size_t>(layer) * layer_stride +
                                (static_cast<size_t>(y) * width + x) * bytes_per_pixel;
    const std::byte * pixel = reinterpret_cast<const std::byte *>(data.data() + pixel_offset);

    glm::vec4 value(0.0f, 0.0f, 0.0f, 1.0f);
    for (uint32_t channel_index = 0; channel_index < channels && channel_index < 4; ++channel_index) {
        const std::byte * channel_ptr = pixel + channel_index * bytes_per_channel;
        value[channel_index] = DecodeValueWithPixelFormat(format, channel_ptr);
    }

    if (format == PixelFormatType::kB8G8R8A8_UNORM || format == PixelFormatType::kB8G8R8A8_SRGB) {
        value = glm::vec4(value.z, value.y, value.x, value.w);
    }

    return value;
}

}

Texture::Texture(RHITextureType type, PixelFormatType format, uint32_t width, uint32_t height, uint32_t mip_levels, uint32_t layers)
    : type_(type),
    width_(width),
    height_(height),
    mip_levels_(mip_levels),
    layers_(layers),
    format_(format),
    dirty_(true) {
    // Always allocate the CPU mirror zero-filled so procedural generators can
    // write into it directly. The mirror is always uploaded on UpdateOnDevice.
    size_t needed_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) *
                         static_cast<size_t>(GetPixelFormatBytesPerPixel(format_)) *
                         static_cast<size_t>(layers_);
    data_.assign(needed_size, 0);
}

void Texture::InitializeFromBinary(std::span<uint8_t> data)
{
    size_t needed_size = static_cast<size_t>(width_) * static_cast<size_t>(height_) *
                         static_cast<size_t>(GetPixelFormatBytesPerPixel(format_)) *
                         static_cast<size_t>(layers_);
    if (data.size() != needed_size) {
        MI_WARN("Texture::InitializeFromBinary(): Incorrect data size. Expected{}, got {}.", needed_size, data.size());
        return;
    }

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

    // Always upload the CPU mirror (zero-filled until filled by a loader /
    // procedural generator). GPU-only textures (e.g. env cubemap filled by a
    // render pass) just eat a one-time zero upload that the pass overwrites.
    Helpers::Upload_Async(queue, device_texture_.Raw(), data_.data(), data_.size(),
        RHITextureLayoutType::kShaderReadOnlyOptimal, RHIGPUAccessFlagBits::kShaderRead);

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

glm::vec4 Texture::Load(uint32_t x, uint32_t y, uint32_t layer) const {
    if (width_ == 0 || height_ == 0) {
        return {};
    }
    if (x >= width_ || y >= height_ || layer >= layers_) {
        MI_WARN("Texture::Load(): out-of-bounds access, texture={}, x={}, y={}, layer={}, size=({}, {}), layers={}",
            GetName(), x, y, layer, width_, height_, layers_);
        return {};
    }

    return LoadTexelFromBinary(data_, format_, width_, height_, x, y, layer);
}

glm::vec4 Texture::Sample(TextureSampleMode mode, glm::vec2 uv, uint32_t layer) const {
    if (width_ == 0 || height_ == 0) {
        return {};
    }
    if (layer >= layers_) {
        MI_WARN("Texture::Sample(): out-of-bounds layer access, texture={}, layer={}, layers={}",
            GetName(), layer, layers_);
        return {};
    }

    const uint32_t channels = GetPixelFormatNumChannels(format_);
    const uint32_t bytes_per_channel = GetPixelFormatNumBytesPerChannel(format_);
    const uint32_t bytes_per_pixel = channels * bytes_per_channel;
    const size_t layer_stride = static_cast<size_t>(width_) * static_cast<size_t>(height_) * bytes_per_pixel;
    const size_t required_bytes = (static_cast<size_t>(layer) + 1) * layer_stride;
    if (data_.size() < required_bytes) {
        MI_WARN("Texture::Sample(): binary data is incomplete, texture={}, expected bytes >= {}, got {}.",
            GetName(), required_bytes, data_.size());
        return {};
    }

    auto sample_texel = [&](int32_t x, int32_t y) {
        return Load(static_cast<uint32_t>(x), static_cast<uint32_t>(y), layer);
    };

    const bool linear = mode == TextureSampleMode::kLinearClamp || mode == TextureSampleMode::kLinearWrap;
    const bool wrap = mode == TextureSampleMode::kNearestWrap || mode == TextureSampleMode::kLinearWrap;

    glm::vec2 uv_sample = uv;
    if (wrap) {
        uv_sample.x = NormalizeUV(uv_sample.x);
        uv_sample.y = NormalizeUV(uv_sample.y);
    } else {
        uv_sample = glm::clamp(uv_sample, glm::vec2(0.0f), glm::vec2(1.0f));
    }

    if (!linear) {
        int32_t ix = static_cast<int32_t>(std::floor(uv_sample.x * static_cast<float>(width_)));
        int32_t iy = static_cast<int32_t>(std::floor(uv_sample.y * static_cast<float>(height_)));
        ix = ResolveTexelCoordinate(ix, static_cast<int32_t>(width_), wrap);
        iy = ResolveTexelCoordinate(iy, static_cast<int32_t>(height_), wrap);
        return sample_texel(ix, iy);
    }

    float px = uv_sample.x * static_cast<float>(width_) - 0.5f;
    float py = uv_sample.y * static_cast<float>(height_) - 0.5f;
    int32_t x0 = static_cast<int32_t>(std::floor(px));
    int32_t y0 = static_cast<int32_t>(std::floor(py));
    int32_t x1 = x0 + 1;
    int32_t y1 = y0 + 1;

    float tx = px - static_cast<float>(x0);
    float ty = py - static_cast<float>(y0);

    x0 = ResolveTexelCoordinate(x0, static_cast<int32_t>(width_), wrap);
    y0 = ResolveTexelCoordinate(y0, static_cast<int32_t>(height_), wrap);
    x1 = ResolveTexelCoordinate(x1, static_cast<int32_t>(width_), wrap);
    y1 = ResolveTexelCoordinate(y1, static_cast<int32_t>(height_), wrap);

    glm::vec4 c00 = sample_texel(x0, y0);
    glm::vec4 c10 = sample_texel(x1, y0);
    glm::vec4 c01 = sample_texel(x0, y1);
    glm::vec4 c11 = sample_texel(x1, y1);

    glm::vec4 cx0 = glm::mix(c00, c10, tx);
    glm::vec4 cx1 = glm::mix(c01, c11, tx);
    return glm::mix(cx0, cx1, ty);
}

MI_NAMESPACE_END
