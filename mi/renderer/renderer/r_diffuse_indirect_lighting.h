/*
 * Created: 2025/10/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_DIFFUSE_INDIRECT_LIGHTING_H
#define MI_R_DIFFUSE_INDIRECT_LIGHTING_H
#include "renderer/mi_renderer.h"
MI_NAMESPACE_BEGIN

class DiffuseIndirectLightingParams;

struct DiffuseIndirectLightingData : public RefCounted<> {
    TRef<RDGTexture> screen_probe_radiance_depth;
    // Exposed for updating persistent data
    TRef<RDGBuffer>  screen_probe_cache_updated_mru_queue_buffer;
    TRef<RDGTexture> tile_screen_probe_header_texture;
    TRef<RDGTexture> radiance;

    // Internally used for state keeping.
    DiffuseIndirectLightingParams * shader_params;
    TRef<RDGBuffer> shading_point_command; // 1 thread per shading point from update rays
    TRef<RDGBuffer> spawn_list_command; // 1 thread per probe spawn list entry

    void Allocate (RenderGraphBuilder & builder, RendererView * view) ;
};

struct DiffuseIndirectLightingPersistentData : public RefCounted<> {
    TRef<RDGTexture> ScreenProbeRadianceDepthTexture;
    TRef<RDGBuffer>  ScreenProbeCacheData;
    TRef<RDGTexture> ScreenProbeCacheRadianceDepthTexture;
    TRef<RDGBuffer>  ScreenProbeCacheMRUQueueBuffer;
    TRef<RDGTexture> TileScreenProbeHeaderTexture;
    TRef<RDGBuffer>  ScreenProbeCacheMRUFlagBuffer;

    bool MakeSureExists (RenderGraphBuilder & builder, glm::uvec2 tile_dimensions, uint32_t header_tile_dimension) ;

    void FinalUpdate (RendererView * view);
};

MI_NAMESPACE_END
#endif //MI_R_DIFFUSE_INDIRECT_LIGHTING_H