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
    glm::vec3 position;
    glm::vec3 direction;
    glm::vec3 up;

    float fov_Y;
    float near_plane;
    float far_plane;
};

MI_NAMESPACE_END
#endif //MI_CAMERA_H
