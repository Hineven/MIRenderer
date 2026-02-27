/*
 * Created: 2025/9/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_MI_LIGHTS_H
#define MI_MI_LIGHTS_H

#include <glm/glm.hpp>

#include <core/common.h>
MI_NAMESPACE_BEGIN

struct DirectionalLight {
    bool enabled {true};
    glm::vec3 direction {};
    glm::vec3 color {1.f, 1.f, 1.f};
    float intensity {1.f};
};

MI_NAMESPACE_END

#endif //MI_MI_LIGHTS_H