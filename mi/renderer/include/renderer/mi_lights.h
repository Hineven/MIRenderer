/*
 * Created: 2025/9/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_MI_LIGHTS_H
#define MI_MI_LIGHTS_H

#include <glm/vec3.hpp>

#include <core/common.h>
MI_NAMESPACE_BEGIN

struct DirectionalLight {
    glm::vec3 direction {};
};

MI_NAMESPACE_END

#endif //MI_MI_LIGHTS_H