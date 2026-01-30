/*
 * Created: 2025/11/9
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "r_direct_lighting.h"

#include "r_persistent.h"
#include "renderer/mi_renderer_view.h"
MI_NAMESPACE_BEGIN

static CVar<bool> CVar_DebugFreezeFrameSeed(
    "r.direct_lighting.debug.freeze_frame_seed",
    "Freeze the frame seed for debugging purposes.",
    false
);

static CVar<float> CVar_ShadowRayLengthMultiplier(
    "r.direct_lighting.shadow_ray_length_multiplier",
    "Multiplier for the shadow ray length for direct lighting occlusion tests.",
    0.998f
);

// TODO currently SSRT is buggy, disabled for now.
static CVar<bool> CVar_SSRT_Disabled(
    "r.direct_lighting.ssrt_disabled",
    "Disable screen space ray tracing. (NOTE: SSRT is buggy for now)",
    true
);

void FillUniformBufferForDirectLighting(RendererView * view, DirectLightingUB* DI_UB) {
    if (CVar_DebugFreezeFrameSeed.Get()) DI_UB->FrameIndex = 0;
    else DI_UB->FrameIndex = view->persistent_data_->frame_index_;
    DI_UB->ShadowRayTMax = view->camera_.far_plane;
    DI_UB->ShadowRayLengthMultiplier = CVar_ShadowRayLengthMultiplier.Get();
}

void FillUniformBufferForHybridTracing(RendererView *view, HybridTracingUB *UB) {
    UB->SSRT_Disabled = CVar_SSRT_Disabled.Get() ? 1 : 0;
    UB->SSRT_RelativeTexelThickness = 1e-4f;
    UB->RayContinuationBackwardBiasFactor = 1e-3f;
    UB->DefaultTMax = view->camera_.far_plane;
}

MI_NAMESPACE_END