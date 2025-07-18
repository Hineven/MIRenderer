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
    kMax
};

MI_NAMESPACE_END
#endif //MI_RENDERER_TYPES_H
