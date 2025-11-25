/*
 * Created: 2025/4/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERER_TYPES_H
#define MI_RENDERER_TYPES_H

#include "core/common.h"
MI_NAMESPACE_BEGIN

enum class RenderableType {
    // Static mesh + transform
    kStaticMeshInstance = 0,
    // Volume primitives + transform
    kVolumePrimitivesInstance,
    // Gaussian radiance field + transform (3D Gaussian Radiance Field)
    kGaussianRadianceFieldInstance,
    kMax
};

FORCEINLINE std::string ToString (RenderableType type) {
    switch (type) {
        case RenderableType::kStaticMeshInstance: return "StaticMeshInstance";
        case RenderableType::kVolumePrimitivesInstance: return "VolumePrimitivesInstance";
        case RenderableType::kGaussianRadianceFieldInstance: return "GaussianRadianceFieldInstance";
        default: return "Unknown";
    }
}

MI_NAMESPACE_END
#endif //MI_RENDERER_TYPES_H
