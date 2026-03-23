/*
 * Created: 2025/11/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_GAUSSIAN_RADIANCE_FIELD_H
#define MI_R_GAUSSIAN_RADIANCE_FIELD_H
#include "renderer/mi_renderer.h"
MI_NAMESPACE_BEGIN

extern CVar<float> CVar_GRF_EmitterIntensityScale;

struct GaussianRadianceFieldViewData : RefCounted<> {
    // Output depth for stochastic GRF rendering (if enabled). You can export this if you like.
    TRef<RDGTexture> stochastic_rendering_depth_;

    // Output opacity for stochastic GRF rendering (if enabled). You can export this if you like.
    TRef<RDGTexture> stochastic_rendering_opacity_;
    TRef<RDGTexture> stochastic_rendering_opacity_large_;
    TRef<RDGTexture> stochastic_rendering_visibility_;

    void Allocate (RenderGraphBuilder & builder, RendererView * view) ;
};

MI_NAMESPACE_END
#endif //MI_R_GAUSSIAN_RADIANCE_FIELD_H