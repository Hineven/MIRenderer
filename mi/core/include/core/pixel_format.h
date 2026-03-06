/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_PIXEL_FORMAT_H
#define MIRENDERER_CORE_PIXEL_FORMAT_H

#include <string>
#include <cassert>

#include "rhi/rhi_common.h"

MI_NAMESPACE_BEGIN

enum class PixelFormatType {
    kUnknown = 0,
    kR8_UINT,
    kR8_UNORM,
    kR8G8_UNORM,
    kB8G8R8A8_UNORM,
    kB8G8R8A8_SRGB,
    kR8G8B8A8_UNORM,
    kR8G8B8A8_SRGB,
    kR16G16B16A16_FLOAT,
    kR16G16_FLOAT,
    kR32G32B32A32_FLOAT,
    kR32G32B32_FLOAT,
    kR32G32_FLOAT,
    kR32_FLOAT,
    kD32_FLOAT,
    kR32G32B32A32_UINT,
    kR32G32B32_UINT,
    kR32G32_UINT,
    kR32_UINT,
    kMax
};

enum class PixelFormatDataType {
    kUnknown = 0,
    kUNORM,
    kSRGB,
    kFLOAT,
    kUINT,
    kMAX
};

FORCEINLINE PixelFormatDataType GetPixelFormatDataType (PixelFormatType type) {
    switch (type) {
        case PixelFormatType::kR8_UNORM:
        case PixelFormatType::kR8G8_UNORM:
        case PixelFormatType::kB8G8R8A8_UNORM:
        case PixelFormatType::kR8G8B8A8_UNORM:
            return PixelFormatDataType::kUNORM;

        case PixelFormatType::kB8G8R8A8_SRGB:
        case PixelFormatType::kR8G8B8A8_SRGB:
            return PixelFormatDataType::kSRGB;

        case PixelFormatType::kR16G16B16A16_FLOAT:
        case PixelFormatType::kR16G16_FLOAT:
        case PixelFormatType::kR32G32B32A32_FLOAT:
        case PixelFormatType::kR32G32B32_FLOAT:
        case PixelFormatType::kR32G32_FLOAT:
        case PixelFormatType::kR32_FLOAT:
        case PixelFormatType::kD32_FLOAT:
            return PixelFormatDataType::kFLOAT;

        case PixelFormatType::kR32G32B32A32_UINT:
        case PixelFormatType::kR32G32B32_UINT:
        case PixelFormatType::kR32G32_UINT:
        case PixelFormatType::kR32_UINT:
        case PixelFormatType::kR8_UINT:
            return PixelFormatDataType::kUINT;

        default:
            assert(false);
            return PixelFormatDataType::kUnknown;
    }
}

FORCEINLINE uint32_t GetPixelFormatNumBytesPerChannel(PixelFormatType type) {
    switch (type) {
        case PixelFormatType::kR8_UNORM:
        case PixelFormatType::kR8_UINT:
        case PixelFormatType::kR8G8_UNORM:
        case PixelFormatType::kB8G8R8A8_UNORM:
        case PixelFormatType::kB8G8R8A8_SRGB:
        case PixelFormatType::kR8G8B8A8_UNORM:
        case PixelFormatType::kR8G8B8A8_SRGB:
            return 1;

        case PixelFormatType::kR16G16B16A16_FLOAT:
        case PixelFormatType::kR16G16_FLOAT:
            return 2;

        case PixelFormatType::kR32G32B32A32_FLOAT:
        case PixelFormatType::kR32G32B32_FLOAT:
        case PixelFormatType::kR32G32_FLOAT:
        case PixelFormatType::kR32_FLOAT:
        case PixelFormatType::kD32_FLOAT:
        case PixelFormatType::kR32G32B32A32_UINT:
        case PixelFormatType::kR32G32B32_UINT:
        case PixelFormatType::kR32G32_UINT:
        case PixelFormatType::kR32_UINT:
            return 4;

        default:
            assert(false);
            return 0;
    }
}

FORCEINLINE uint32_t GetPixelFormatNumChannels (PixelFormatType type) {
    switch (type) {
        case PixelFormatType::kR8_UNORM:
        case PixelFormatType::kR8_UINT:
        case PixelFormatType::kR32_FLOAT:
        case PixelFormatType::kD32_FLOAT:
        case PixelFormatType::kR32_UINT:
            return 1;

        case PixelFormatType::kR8G8_UNORM:
        case PixelFormatType::kR16G16_FLOAT:
        case PixelFormatType::kR32G32_FLOAT:
        case PixelFormatType::kR32G32_UINT:
            return 2;

        case PixelFormatType::kR32G32B32_FLOAT:
        case PixelFormatType::kR32G32B32_UINT:
            return 3;

        case PixelFormatType::kB8G8R8A8_UNORM:
        case PixelFormatType::kB8G8R8A8_SRGB:
        case PixelFormatType::kR8G8B8A8_UNORM:
        case PixelFormatType::kR8G8B8A8_SRGB:
        case PixelFormatType::kR16G16B16A16_FLOAT:
        case PixelFormatType::kR32G32B32A32_FLOAT:
        case PixelFormatType::kR32G32B32A32_UINT:
            return 4;

        default:
            assert(false);
            return 0;
    }
}

FORCEINLINE bool IsDepthStencilPixelFormat (PixelFormatType format) {
    return format == PixelFormatType::kD32_FLOAT;
}

FORCEINLINE const char * GetPixelFormatName (PixelFormatType type) {
    switch (type) {
        case PixelFormatType::kUnknown:
            return "Unknown";
        case PixelFormatType::kR8_UINT:
            return "R8_UINT";
        case PixelFormatType::kR8_UNORM:
            return "R8_UNORM";
        case PixelFormatType::kR8G8_UNORM:
            return "R8G8_UNORM";
        case PixelFormatType::kB8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case PixelFormatType::kB8G8R8A8_SRGB:
            return "B8G8R8A8_SRGB";
        case PixelFormatType::kR8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        case PixelFormatType::kR8G8B8A8_SRGB:
            return "R8G8B8A8_SRGB";
        case PixelFormatType::kR16G16B16A16_FLOAT:
            return "R16G16B16A16_FLOAT";
        case PixelFormatType::kR16G16_FLOAT:
            return "R16G16_FLOAT";
        case PixelFormatType::kR32G32B32A32_FLOAT:
            return "R32G32B32A32_FLOAT";
        case PixelFormatType::kR32G32B32_FLOAT:
            return "R32G32B32_FLOAT";
        case PixelFormatType::kR32G32_FLOAT:
            return "R32G32_FLOAT";
        case PixelFormatType::kR32_FLOAT:
            return "R32_FLOAT";
        case PixelFormatType::kD32_FLOAT:
            return "D32_FLOAT";
        case PixelFormatType::kR32G32B32A32_UINT:
            return "R32G32B32A32_UINT";
        case PixelFormatType::kR32G32B32_UINT:
            return "R32G32B32_UINT";
        case PixelFormatType::kR32G32_UINT:
            return "R32G32_UINT";
        case PixelFormatType::kR32_UINT:
            return "R32_UINT";
        default:
            assert(false);
            return "Unknown";
    }
}

FORCEINLINE std::string ToString (PixelFormatType type) {
    return GetPixelFormatName(type);
}

FORCEINLINE bool IsFloatPixelFormat (PixelFormatType type) {
    auto data_type = GetPixelFormatDataType(type);
    return data_type == PixelFormatDataType::kFLOAT ||
           data_type == PixelFormatDataType::kUNORM ||
           data_type == PixelFormatDataType::kSRGB;
}

FORCEINLINE bool IsUIntPixelFormat (PixelFormatType type) {
    return GetPixelFormatDataType(type) == PixelFormatDataType::kUINT;
}

FORCEINLINE bool IsPixelFormat4ComponentFloat (PixelFormatType type) {
    return IsFloatPixelFormat(type) &&
           GetPixelFormatNumChannels(type) == 4;
}

FORCEINLINE uint32_t GetPixelFormatBytesPerPixel(PixelFormatType type) {
    uint32_t bytesPerChannel = GetPixelFormatNumBytesPerChannel(type);
    uint32_t numChannels = GetPixelFormatNumChannels(type);
    return bytesPerChannel * numChannels;
}

MI_NAMESPACE_END

#endif //MIRENDERER_CORE_PIXEL_FORMAT_H
