/*
 * Created: 2025/11/9
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_VOLUME_DIRECT_LIGHTING_H
#define MI_R_VOLUME_DIRECT_LIGHTING_H
#include <core/refcounted.h>
#include <rdg/rdg_fwd.h>
#include <renderer/mi_renderer_fwd.h>
MI_NAMESPACE_BEGIN

struct VolumeDirectLightingData : public RefCounted<> {
    TRef<RDGTexture> radiance;

    void Allocate (RenderGraphBuilder & builder, RendererView * view) ;
};

MI_NAMESPACE_END
#endif //MI_R_VOLUME_DIRECT_LIGHTING_H