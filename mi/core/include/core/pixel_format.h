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
    kMax
};

MI_NAMESPACE_END

#endif //MIRENDERER_CORE_PIXEL_FORMAT_H
