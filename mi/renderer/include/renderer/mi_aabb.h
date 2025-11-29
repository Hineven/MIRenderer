/*
 * Created: 2025/7/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_AABB_H
#define MI_AABB_H
#include "mi_transform.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

struct AABB {
    glm::vec3 min {INFINITY};
    glm::vec3 max {-INFINITY};

    AABB() = default;
    AABB(const glm::vec3 &min, const glm::vec3 &max) : min(min), max(max) {}

    FORCEINLINE bool IsValid() const {
        return min.x <= max.x && min.y <= max.y && min.z <= max.z;
    }

    FORCEINLINE glm::vec3 GetCenter() const {
        return (min + max) * 0.5f;
    }

    FORCEINLINE static AABB Empty() {
        return {};
    }

    FORCEINLINE static AABB Merge (const AABB &a, const AABB &b) {
        return AABB(
            glm::min(a.min, b.min),
            glm::max(a.max, b.max)
        );
    }

    FORCEINLINE static AABB FromCenterAndHalfSize(const glm::vec3 &center, const glm::vec3 &half_size) {
        return AABB(
            center - half_size,
            center + half_size
        );
    }

    FORCEINLINE AABB Transformed (const glm::mat4x3 & matrix) const {
        glm::vec3 corners[8] = {
            {min.x, min.y, min.z},
            {min.x, min.y, max.z},
            {min.x, max.y, min.z},
            {min.x, max.y, max.z},
            {max.x, min.y, min.z},
            {max.x, min.y, max.z},
            {max.x, max.y, min.z},
            {max.x, max.y, max.z},
        };
        AABB result = Empty();
        for (int i=0;i<8;i++) {
            glm::vec3 transformed_corner = matrix * glm::vec4(corners[i], 1.0f);
            result.Encapsulate(transformed_corner);
        }
        return result;
    }

    FORCEINLINE AABB Transformed (const Transform & transform) const {
        return Transformed(transform.GetToWorldTransformMatrix());
    }

    FORCEINLINE void Encapsulate (const glm::vec3 & point) {
        min = glm::min(min, point);
        max = glm::max(max, point);
    }
    FORCEINLINE void Encapsulate (const AABB & box) {
        min = glm::min(min, box.min);
        max = glm::max(max, box.max);
    }
};

MI_NAMESPACE_END
#endif //MI_AABB_H
