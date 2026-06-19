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
    // Volume Grid (with Super Volume Grid)
    kVolumeGridInstance,
    // GigaVoxel (VC/TFC/LFC) + transform. A single GigaVoxelInstance manages
    // many chunks internally; it occupies one RenderableIndex slot.
    kGigaVoxelInstance,
    kMax
};

FORCEINLINE std::string ToString (RenderableType type) {
    switch (type) {
        case RenderableType::kStaticMeshInstance: return "StaticMeshInstance";
        case RenderableType::kVolumeGridInstance: return "VolumeGridInstance";
        case RenderableType::kGigaVoxelInstance: return "GigaVoxelInstance";
        default: return "Unknown";
    }
}

MI_NAMESPACE_END
#endif //MI_RENDERER_TYPES_H
