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

    FORCEINLINE Transform Translated (const glm::vec3 & translation) const {
        Transform result = *this;
        result.position += translation;
        return result;
    }

    FORCEINLINE void Scale (const glm::vec3 & ext_scale) {
        this->scale *= ext_scale;
    }

    FORCEINLINE Transform Scaled (const glm::vec3 & ext_scale) const {
        Transform result = *this;
        result.scale *= ext_scale;
        return result;
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

    FORCEINLINE glm::mat4x3 GetToLocalTransformMatrix() const {
        glm::mat4 scaleMatrix = glm::scale(glm::mat4(1.0f), 1.f / scale);
        glm::mat4 rotationMatrix = glm::mat4(1.0f);
        rotationMatrix = glm::rotate(rotationMatrix, -rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
        rotationMatrix = glm::rotate(rotationMatrix, -rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
        rotationMatrix = glm::rotate(rotationMatrix, -rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
        glm::mat4 translationMatrix = glm::translate(glm::mat4(1.0f), -position);
        glm::mat4 transform = scaleMatrix * rotationMatrix * translationMatrix;
        return glm::mat4x3(transform);
    }

    FORCEINLINE void Rotate (const glm::vec3 & euler_angles) {
        rotation += euler_angles;
    }

    FORCEINLINE Transform Rotated (const glm::vec3 & euler_angles) const {
        Transform result = *this;
        result.rotation += euler_angles;
        return result;
    }

    FORCEINLINE void RotateAbout (float angle, const glm::vec3 & axis) {
        glm::quat q = glm::angleAxis(angle, glm::normalize(axis));
        auto current = glm::quat(rotation);
        glm::quat result = q * current;
        rotation = glm::eulerAngles(result);
    }

    FORCEINLINE Transform RotatedAbout (float angle, const glm::vec3 & axis) const {
        glm::quat q = glm::angleAxis(angle, glm::normalize(axis));
        auto current = glm::quat(rotation);
        glm::quat result = q * current;
        Transform t = *this;
        t.rotation = glm::eulerAngles(result);
        return t;
    }

    FORCEINLINE static Transform Identity () {
        return Transform();
    }


    static Transform FromMatrix (glm::mat4) ;
};

MI_NAMESPACE_END
#endif //MI_TRANSFORM_H
