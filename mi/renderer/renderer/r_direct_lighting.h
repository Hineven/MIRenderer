/*
 * Created: 2025/11/9
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_DIRECT_LIGHTING_H
#define MI_R_DIRECT_LIGHTING_H
#include <renderer/mi_renderer_fwd.h>
MI_NAMESPACE_BEGIN

struct DirectLightingUB {
    uint32_t FrameIndex;
    float ShadowRayTMax;
    float ShadowRayLengthMultiplier;
    uint32_t Unused;
};

struct HybridTracingUB {
    uint32_t SSRT_Disabled;
    float SSRT_RelativeTexelThickness;
    float RayContinuationBackwardBiasFactor;
    float DefaultTMax;
};


void FillUniformBufferForDirectLighting (RendererView * view, DirectLightingUB * UB) ;
void FillUniformBufferForHybridTracing (RendererView * view, HybridTracingUB * UB) ;

MI_NAMESPACE_END
#endif //MI_R_DIRECT_LIGHTING_H
