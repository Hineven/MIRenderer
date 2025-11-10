/*
 * Created: 2025/11/8
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_VOLUME_INDIRECT_LIGHTING_H
#define MI_R_VOLUME_INDIRECT_LIGHTING_H
#include "renderer/mi_renderer.h"
MI_NAMESPACE_BEGIN

struct VolumeIndirectLightingData : public RefCounted<> {
    TRef<RDGBuffer> active_volume_probe_count;
    TRef<RDGBuffer> active_volume_probe_list_buffer;
    TRef<RDGBuffer> volume_probe_next_mru_queue_buffer;
    TRef<RDGTexture> volume_probe_radiance_depth;

    TRef<RDGTexture> radiance;

    void Allocate (RenderGraphBuilder & builder, RendererView * view) ;
};

struct VolumeIndirectLightingPersistentData : public RefCounted<> {
    TRef<RDGBuffer> ActiveVolumeProbeCount;
    TRef<RDGBuffer> ActiveVolumeProbeListBuffer;
    TRef<RDGBuffer>  VolumeProbeMRUQueueBuffer;

    TRef<RDGTexture> VolumeProbeRadianceDepthTexture;
    TRef<RDGTexture> VolumeProbeHeaderTexture;

    bool MakeSureExists (RenderGraphBuilder & builder, glm::uvec2 tile_dimensions, uint32_t header_tile_dimension) ;

    void FinalUpdate (RendererView * view);
};

MI_NAMESPACE_END
#endif //MI_R_VOLUME_INDIRECT_LIGHTING_H