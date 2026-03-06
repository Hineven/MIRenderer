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

    // Radians
    float fov_Y {1.f};
    float near_plane {0.1f};
    float far_plane {1000.f};

    FORCEINLINE bool operator == (const Camera & other) const {
        return position == other.position &&
               direction == other.direction &&
               up == other.up &&
               fov_Y == other.fov_Y &&
               near_plane == other.near_plane &&
               far_plane == other.far_plane;
    }

    FORCEINLINE glm::vec3 GetRight () const {
        return glm::normalize(glm::cross(direction, up));
    }

    FORCEINLINE glm::vec3 GetScaledRight (float aspect_ratio) const {
        return GetRight() * aspect_ratio * tanf(fov_Y * 0.5f);
    }

    // Different from the 'up' member (which is essentially an indicator for up direction),
    // ortho_up is always perpendicular to direction and right.
    FORCEINLINE glm::vec3 GetOrthoUp () const {
        return glm::normalize(glm::cross(GetRight(), direction));
    }

    // Returns ortho_up scaled by tan(fov_Y * 0.5)
    FORCEINLINE glm::vec3 GetScaledUp () const {
        return GetOrthoUp() * tanf(fov_Y * 0.5f);
    }

    // Map (film space) NDC to a direction in world space
    FORCEINLINE glm::vec3 NDC2ToCameraDirectionUnnormalized (glm::vec2 NDC2, float aspect) const {
        glm::vec3 UnnormalizedDirection = GetScaledRight(aspect) * NDC2.x + GetScaledUp() * NDC2.y + direction;
        return UnnormalizedDirection;
    }

    FORCEINLINE float ZDepthToLinearDepth(float z) const
    {
        return far_plane * near_plane / (far_plane - z * (far_plane - near_plane));
    }

    FORCEINLINE float ReversedZDepthToLinearDepth(float reversed_z) const
    {
        return ZDepthToLinearDepth(1.0f - reversed_z);
    }

    FORCEINLINE glm::vec3 RecoverWorldPositionNDC2(glm::vec2 NDC2, float linear_depth, float aspect) const
    {
        return position + NDC2ToCameraDirectionUnnormalized(NDC2, aspect) * linear_depth;
    }
};

MI_NAMESPACE_END
#endif //MI_CAMERA_H
