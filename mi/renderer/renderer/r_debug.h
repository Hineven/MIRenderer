/*
 * Created: 2025/12/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_DEBUG_H
#define MI_R_DEBUG_H

#include <renderer/mi_renderer_fwd.h>

MI_NAMESPACE_BEGIN

struct DebugPersistentData : public RefCounted<> {
    TRef<RDGBuffer> visualize_spatial_positions_;
    TRef<RDGBuffer> visualize_spatial_positions_count_;

    DebugPersistentData();
    ~DebugPersistentData();

    void MakeSureExists (RendererView * view, RenderGraphBuilder & builder) ;
};

MI_NAMESPACE_END
#endif //MI_R_DEBUG_H