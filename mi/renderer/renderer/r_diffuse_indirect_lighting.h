/*
 * Created: 2025/10/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_DIFFUSE_INDIRECT_LIGHTING_H
#define MI_R_DIFFUSE_INDIRECT_LIGHTING_H
#include "renderer/mi_renderer.h"
MI_NAMESPACE_BEGIN

struct DiffuseIndirectLightingPersistentData : public RefCounted<> {
    TRef<RDGTexture> ScreenProbeRadianceDepthTexture;
    TRef<RDGBuffer>  ScreenProbeCacheData;
    TRef<RDGTexture> ScreenProbeCacheRadianceDepthTexture;
    TRef<RDGBuffer>  ScreenProbeCacheMRUQueueBuffer;
    TRef<RDGTexture> TileScreenProbeHeaderTexture;

    bool MakeSureExists (RenderGraphBuilder & builder, glm::uvec2 tile_dimensions, uint32_t header_tile_dimension) ;
};

MI_NAMESPACE_END
#endif //MI_R_DIFFUSE_INDIRECT_LIGHTING_H