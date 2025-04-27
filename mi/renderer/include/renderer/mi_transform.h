/*
 * Created: 2025/4/13
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_TRANSFORM_H
#define MI_TRANSFORM_H
#include <glm/glm.hpp>
#include <glm/trigonometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include "core/common.h"
MI_NAMESPACE_BEGIN

struct Transform {
    glm::vec3 position {};
    // Axis angles (in radians)
    glm::vec3 rotation {};
    glm::vec3 scale {1.f};

    FORCEINLINE void Translate (const glm::vec3 & translation) {
        position += translation;
    }

    FORCEINLINE void Scale (const glm::vec3 & ext_scale) {
        this->scale *= ext_scale;
    }

    FORCEINLINE glm::mat4x3 GetToWorldTransformMatrix() const {
        glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), scale);
        glm::mat4 rotationMatrix = glm::mat4(1.0f);
        rotationMatrix = glm::rotate(rotationMatrix, rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
        rotationMatrix = glm::rotate(rotationMatrix, rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
        rotationMatrix = glm::rotate(rotationMatrix, rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
        glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), position);
        glm::mat4 transform = translationMatrix * rotationMatrix * scaleMatrix;
        return glm::mat4x3(transform);
    }

    static Transform FromMatrix (glm::mat4) ;
};

MI_NAMESPACE_END
#endif //MI_TRANSFORM_H
