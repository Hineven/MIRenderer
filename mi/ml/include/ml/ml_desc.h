/*
 * Created: 2024/7/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_MI_DESC_H
#define MIRENDERER_MI_DESC_H
#include <span>
#include "rhi/rhi_types.h"
#include "core/common.h"
#include "core/refcounted.h"
#include "mi/mi_fwd.h"
MI_NAMESPACE_BEGIN

struct GeometryDesc {
    std::span<std::span<const void*>> vertex_buffers;

    RHIIndexType index_type;
    std::span<void*> index_buffer;

    struct GeometryVertexBindingDesc {
        int stride;
        int offset;
        int buffer_index;
    };

    GeometryVertexBindingDesc position;
    GeometryVertexBindingDesc normal;
    GeometryVertexBindingDesc tangent;
    // Support up to 2 UVs
    GeometryVertexBindingDesc uv, uv1;
};

struct MeshCreateDesc {
    std::string name;
    std::span<GeometryDesc> geometries;
    std::span<Material*> materials;
};

MI_NAMESPACE_END
#endif //MIRENDERER_MI_DESC_H
