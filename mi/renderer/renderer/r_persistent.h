/*
 * Created: 2025/10/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_PERSISTENT_H
#define MI_R_PERSISTENT_H

#include <renderer/mi_renderer.h>
#include "r_view_common.h"

MI_NAMESPACE_BEGIN

struct DiffuseIndirectLightingPersistentData;

// The data kept across frames for a view.
struct RendererViewPersistentData {

    RendererViewPersistentData() ;
    ~RendererViewPersistentData() ;

    void Init ();
    void FinalUpdate (RendererView * view);

    TRef<RDGTexture> prev_G_depth;
    TRef<RDGTexture> prev_G_normal;

    TRef<RDGTexture> prev_denoised_diffuse_direct_lighting;

    TRef<RDGTexture> prev_radiance_;

    TRef<RDGTexture> path_tracing_film_;

    Camera prev_camera {};
    CameraParameters prev_camera_parameters_ {};

    uint32_t view_index {};
    uint32_t frame_index_ {};

    TRef<DiffuseIndirectLightingPersistentData> diffuse_indirect_lighting_persistent_data_;

    Scene * prev_scene_ {};
};


MI_NAMESPACE_END

#endif //MI_R_PERSISTENT_H