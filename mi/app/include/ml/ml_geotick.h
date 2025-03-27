/*
 * Created: 2024/9/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_MI_GEOTICK_H
#define MIRENDERER_MI_GEOTICK_H

#include <core/common.h>
MI_NAMESPACE_BEGIN

// Context for running geometry ticks. Meshes can be informed with their positions in the scene though this context.
// Thus, they can update their geometries or streaming virtualized geometries.
class GeometryTickContext {
public:
    GeometryTickContext() = default;
    virtual ~GeometryTickContext() = default;

};

MI_NAMESPACE_END

#endif //MIRENDERER_MI_GEOTICK_H
