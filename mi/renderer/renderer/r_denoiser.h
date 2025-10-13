/*
 * Created: 2025/10/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_DENOISER_H
#define MI_R_DENOISER_H
#include <renderer/mi_cvar.h>
#include <rdg/rdg_resource.h>
MI_NAMESPACE_BEGIN

struct DenoiserPersistentData : RefCounted<> {
    // Denoiser history for diffuse lighting
    TRef<RDGTexture> prev_lighting_history_length;
    // RELAX: this is temporally accumulated (rgb + luminance variance)
    TRef<RDGTexture> prev_prefiltered_diffuse_direct_lighting;
    TRef<RDGTexture> prev_denoised_diffuse_indirect_lighting;

    bool MakeSureExists(RendererView * view, RenderGraphBuilder & builder) ;
};

MI_NAMESPACE_END
#endif //MI_R_DENOISER_H