/*
 * Created: 2025/9/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_DIFFUSE_DIRECT_LIGHTING_H
#define MI_R_DIFFUSE_DIRECT_LIGHTING_H
#include <renderer/mi_renderer_fwd.h>

MI_NAMESPACE_BEGIN

struct DiffuseDirectLightingData : public RefCounted<> {
    TRef<RDGTexture> radiance;

    void Allocate (RenderGraphBuilder & builder, RendererView * view) ;
};

MI_NAMESPACE_END

#endif //MI_R_DIFFUSE_DIRECT_LIGHTING_H