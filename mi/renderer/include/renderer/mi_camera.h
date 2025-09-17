/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_CAMERA_H
#define MI_CAMERA_H
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

struct Camera {
    glm::vec3 position {};
    glm::vec3 direction {0, 0, -1};
    glm::vec3 up {0, 1, 0};

    float fov_Y {1.f};
    float near_plane {0.1f};
    float far_plane {1000.f};

    glm::mat4 proj;
    glm::mat4 view;

    glm::vec3 GetRight () const {
        return glm::normalize(glm::cross(direction, up));
    }
};

MI_NAMESPACE_END
#endif //MI_CAMERA_H
