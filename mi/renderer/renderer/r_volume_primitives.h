/*
 * Created: 2025/10/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_VOLUME_PRIMITIVES_H
#define MI_R_VOLUME_PRIMITIVES_H

#include <renderer/mi_renderer_view.h>

MI_NAMESPACE_BEGIN

struct VolumePrimitivesViewData : RefCounted<> {
    // Min-max values for the rendered volume segment
    TRef<RDGTexture> G_volume_min_max_;
    // Volume density
    TRef<RDGTexture> G_volume_density_;
    // Albedo of the volume segment
    TRef<RDGTexture> G_volume_color_;
    // Volume density in fourier term
    TRef<RDGTexture> G_volume_density_fourier_;
    // Albedo of the volume segment multiplied by density in fourier term
    TRef<RDGTexture> G_volume_weighted_color_fourier_;
    // CDF of recorded volume segment
    TRef<RDGTexture> G_volume_cdf_attenuation_;
    // Sampled volume data for light sampling
    TRef<RDGTexture> volume_sample_color_and_linear_depth_;
    // Sampled volume data for transmittance and pdf
    TRef<RDGTexture> volume_sample_transmittance_and_pdf_;
    // Representative depth and variation for each volume pixel (for reprojection)
    TRef<RDGTexture> volume_representative_depth_and_variation_;
    void Allocate(
        RenderGraphBuilder & builder, RendererView * view
    );
};

struct VolumePrimitivesViewPersistentData : RefCounted<> {
    // Denoiser history for volume primitives lighting
    TRef<RDGTexture> prev_volume_representative_depth_and_variation_;

    bool MakeSureExists(RendererView * view, RenderGraphBuilder & builder) ;

    void FinalUpdate(RendererView * view);
};

MI_NAMESPACE_END

#endif //MI_R_VOLUME_PRIMITIVES_H