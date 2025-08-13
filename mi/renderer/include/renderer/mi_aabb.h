/*
 * Created: 2025/7/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_AABB_H
#define MI_AABB_H
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
};

MI_NAMESPACE_END
#endif //MI_AABB_H
