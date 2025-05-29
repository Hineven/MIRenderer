/*
 * Created: 2025/4/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_transform.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include "core/infra.h"

MI_NAMESPACE_BEGIN
Transform Transform::FromMatrix(glm::mat4 to_world) {
    glm::vec3 scale;
    glm::quat orientation;
    glm::vec3 translation;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::decompose(to_world, scale, orientation, translation, skew, perspective);
    mi_warning(glm::length(skew) < 0.005f, "Transform::FromMatrix: Skew {} is not zero. Discarding that.", glm::length(skew));
    mi_warning(perspective == glm::vec4(0, 0, 0, 1),
        "Transform::FromMatrix: Perspective ({}, {}, {}, {}) is non standard. Discarding that.",
        perspective.x, perspective.y, perspective.z, perspective.w);
    Transform transform;
    transform.scale = scale;
    transform.rotation = glm::eulerAngles(orientation);
    transform.position = translation;
    return transform;
}


MI_NAMESPACE_END