/*
 * Created: 2026/2/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_DIRECTIONAL_LIGHT_H
#define MI_R_DIRECTIONAL_LIGHT_H

#include <algorithm>

#include <renderer/mi_renderer_view.h>

#include "../shaders/shared/SharedDirectionalLight.hlsl"

MI_NAMESPACE_BEGIN

FORCEINLINE void FillUniformBufferForDirectionalLight(RendererView *view, DirectionalLightUniform *UB) {
    auto & directional_light = view->scene_->directional_light_;
    auto light_direction = directional_light.direction;
    auto light_direction_len2 = glm::dot(light_direction, light_direction);
    if (directional_light.enabled && light_direction_len2 > 1e-8f) {
        UB->ToLightDirection = -glm::normalize(light_direction);
        UB->Enabled = 1;
    } else {
        UB->ToLightDirection = glm::vec3(0.f);
        UB->Enabled = 0;
    }
    UB->Irradiance = directional_light.color * std::max(directional_light.intensity, 0.f);
    UB->Padding = 0;
}

MI_NAMESPACE_END

#endif //MI_R_DIRECTIONAL_LIGHT_H
