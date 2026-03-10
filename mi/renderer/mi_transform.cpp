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
    Transform transform;

    // Translation
    transform.position = glm::vec3(to_world[3]);

    // Extract 3x3
    glm::mat3 M(to_world);

    // Scale from column lengths
    glm::vec3 scale;
    scale.x = glm::length(glm::vec3(M[0]));
    scale.y = glm::length(glm::vec3(M[1]));
    scale.z = glm::length(glm::vec3(M[2]));

    const float eps = 1e-6f;
    if (scale.x < eps || scale.y < eps || scale.z < eps) {
        mi_warning(false, "Transform::FromMatrix: Degenerate scale ({}, {}, {}).",
                   scale.x, scale.y, scale.z);
        transform.scale = glm::max(scale, glm::vec3(eps));
        transform.rotation = glm::vec3(0.0f);
        return transform;
    }

    // Normalize columns to get rotation matrix
    glm::mat3 R;
    R[0] = glm::vec3(M[0]) / scale.x;
    R[1] = glm::vec3(M[1]) / scale.y;
    R[2] = glm::vec3(M[2]) / scale.z;

    // Fix reflection: move sign into one scale component
    if (glm::determinant(R) < 0.0f) {
        if (scale.x >= scale.y && scale.x >= scale.z) {
            scale.x = -scale.x; R[0] = -R[0];
        } else if (scale.y >= scale.x && scale.y >= scale.z) {
            scale.y = -scale.y; R[1] = -R[1];
        } else {
            scale.z = -scale.z; R[2] = -R[2];
        }
    }

    // Rotation from proper rotation matrix 
    glm::quat q = glm::normalize(glm::quat_cast(R));

    transform.scale = scale;
    transform.rotation = glm::eulerAngles(q); // 弧度

    return transform;
}

MI_NAMESPACE_END