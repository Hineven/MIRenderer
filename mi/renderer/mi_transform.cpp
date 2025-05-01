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
    mi_warning(glm::length(skew) < 0.001f, "Transform::FromMatrix: Skew is not zero. Discarding that.");
    mi_warning(glm::length(perspective) < 0.001f, "Transform::FromMatrix: Perspective is not zero. Discarding that.");
    Transform transform;
    transform.scale = scale;
    transform.rotation = glm::eulerAngles(orientation);
    transform.position = translation;
    return transform;
}


MI_NAMESPACE_END