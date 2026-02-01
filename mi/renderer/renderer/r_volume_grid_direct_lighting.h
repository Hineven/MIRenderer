/*
* Created: 2025/12/2
 * Author: Exploring Air Joe
 * See LICENSE for licensing.
 */

#ifndef R_VOLUME_GRID_DIRECT_LIGHTING_H
#define R_VOLUME_GRID_DIRECT_LIGHTING_H

#include <core/refcounted.h>
#include <rdg/rdg_fwd.h>
#include <renderer/mi_renderer_fwd.h>

MI_NAMESPACE_BEGIN

struct VolumeGridDirectLightingData : public RefCounted<> {
    TRef<RDGTexture> sampled_color_and_depth;
    TRef<RDGTexture> radiance;

    void Allocate (RenderGraphBuilder & builder, RendererView * view) ;
};

MI_NAMESPACE_END

#endif //R_VOLUME_GRID_DIRECT_LIGHTING_H
