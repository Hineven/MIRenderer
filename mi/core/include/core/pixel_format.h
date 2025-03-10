/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_PIXEL_FORMAT_H
#define MIRENDERER_CORE_PIXEL_FORMAT_H

#include "rhi/rhi_common.h"

MI_NAMESPACE_BEGIN

enum class PixelFormatType {
    kUnknown,
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
    kR32G32_UINT,
    kR32_UINT,
    kMax
};

FORCEINLINE bool IsDepthStencilPixelFormat (PixelFormatType format) {
    return format == PixelFormatType::kD32_FLOAT;
}

FORCEINLINE const char * GetPixelFormatName (PixelFormatType type) {
    switch (type) {
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
        case PixelFormatType::kR32G32_UINT:
            return "R32G32_UINT";
        case PixelFormatType::kR32_UINT:
            return "R32_UINT";
        default:
            return "Unknown";
    }
}

FORCEINLINE bool IsFloatPixelFormat (PixelFormatType type) {
    switch (type) {
        case PixelFormatType::kR8G8B8A8_UNORM:
        case PixelFormatType::kR8G8B8A8_SRGB:
        case PixelFormatType::kR16G16B16A16_FLOAT:
        case PixelFormatType::kR16G16_FLOAT:
        case PixelFormatType::kR32G32B32A32_FLOAT:
        case PixelFormatType::kR32G32B32_FLOAT:
        case PixelFormatType::kR32G32_FLOAT:
        case PixelFormatType::kR32_FLOAT:
            return true;
        default: return false;
    }
}

FORCEINLINE bool IsUIntPixelFormat (PixelFormatType type) {
    switch (type) {
        case PixelFormatType::kR32G32B32A32_UINT:
        case PixelFormatType::kR32G32_UINT:
        case PixelFormatType::kR32_UINT:
            return true;
        default: return false;
    }
}

MI_NAMESPACE_END

#endif //MIRENDERER_CORE_PIXEL_FORMAT_H
