/*
 * Created: 2025/10/9
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_R_WORLD_RADIANCE_CACHE_H
#define MI_R_WORLD_RADIANCE_CACHE_H
#include <renderer/mi_cvar.h>
#include <rdg/rdg_resource.h>
MI_NAMESPACE_BEGIN

struct HashGridPersistentData : RefCounted<> {
    TRef<RDGBuffer> free_tile_count;
    TRef<RDGBuffer> free_tile_list_buffer;
    // TRef<RDGBuffer> bucket_hash_;
    // TRef<RDGBuffer> bucket_tile_index_;
    TRef<RDGBuffer> tile_timestamp_buffer;
    TRef<RDGBuffer> tile_buchet_hash_buffer;
    TRef<RDGBuffer> cell_value_buffer;


    bool MakeSureExists(RendererView * view, RenderGraphBuilder & builder) ;
};

MI_NAMESPACE_END
#endif //MI_R_WORLD_RADIANCE_CACHE_H