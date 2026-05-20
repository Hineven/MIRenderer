/*
 * Created: 2025/10/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_PERSISTENT_H
#define MI_R_PERSISTENT_H

#include <renderer/mi_renderer.h>
#include "dlss/dlss_rr_context.h"
#include "r_view_common.h"

MI_NAMESPACE_BEGIN
    // The data kept across frames for a view.
struct RendererViewPersistentData {

    RendererViewPersistentData() ;
    ~RendererViewPersistentData() ;

    void Init ();
    void FinalUpdate (RendererView * view);

    // Denoised results from last frame
    TRef<RDGTexture> prev_radiance_;
    // Denoised results from last frame (shaded radiance without emission)
    TRef<RDGTexture> prev_shaded_radiance_no_emission_;
    // Denoised volume radiance from last frame
    TRef<RDGTexture> prev_shaded_volume_radiance_;
    // TAA-resolved radiance from last frame
    TRef<RDGTexture> prev_taa_radiance_;
     // Per view persistent data
     TRef<DebugPersistentData> debug_persistent_data_;

    TRef<RDGTexture> path_tracing_film_;


    // Keep track of camera parameters from the previous frame
    Camera prev_camera {};
    CameraParameters prev_camera_parameters_ {};

    // Jitter used in previous frame (in NDC space per-axis)
    glm::vec2 prev_camera_jitter_ {};

    // History renderable transforms for motion vectors
    std::vector<glm::mat4x3> prev_renderable_transforms_;

    uint32_t view_index_ {};
    uint32_t frame_index_ {};

    TRef<GeometryBufferPersistentData> g_buffer_data_;
    TRef<VolumePrimitivesViewPersistentData> volume_primitives_view_persistent_data_;
    TRef<DenoiserPersistentData> denoiser_persistent_data_;
    TRef<DiffuseIndirectLightingPersistentData> diffuse_indirect_lighting_persistent_data_;
    TRef<VolumeIndirectLightingPersistentData> volume_indirect_lighting_persistent_data_;
    TRef<LightStructurePersistentData> light_structure_persistent_data_;
    TRef<HashGridPersistentData> hash_grid_persistent_data_;

    TRef<DLSSRRContext> dlss_rr_context_;

    Scene * prev_scene_ {};
};


MI_NAMESPACE_END

#endif //MI_R_PERSISTENT_H