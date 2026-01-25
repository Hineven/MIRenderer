/*
 * Created: 2025/11/30
 * Author:  hineven
 * See LICENSE for licensing.
 */
// This file is exposed to other modules for necessary access into G-buffers.
#ifndef MI_R_GEOMETRY_BUFFER_H
#define MI_R_GEOMETRY_BUFFER_H
#include <rdg/rdg_fwd.h>
#include <renderer/mi_renderer_fwd.h>
MI_NAMESPACE_BEGIN

struct GeometryBufferData : public RefCounted<> {
    // Visibility buffer
    // 0: Renderable index, 1: Descriptor Index (8bits) + Primitive Index (24bits)
    // 2, 3: Barycentrics (yz)
    TRef<RDGTexture> G_visibility_;
    // Depth buffer for deferred shading. Reversed-Z D32 float
    TRef<RDGTexture> G_depth_;

    // Decoded G-buffers
    TRef<RDGTexture> G_albedo_;
    // Shading normal as well as the default normal
    TRef<RDGTexture> G_normal_;
    // GeometryNormal. Specially, this is an octahedron packed uint with high precision (PackGeometryNormal in hlsl)
    TRef<RDGTexture> G_geometry_normal_;
    TRef<RDGTexture> G_emission_;
    TRef<RDGTexture> G_metallic_roughness_;
    TRef<RDGTexture> G_motion_vector_;

    // Flags (R8Uint)
    TRef<RDGTexture> G_flags_;
    // Transmittance for visible volume primitives in front of solid meshes
    TRef<RDGTexture> G_transmittance_;

    GeometryBufferData();
    ~GeometryBufferData() override;
    void Allocate(
        RenderGraphBuilder & builder, RendererView * view
    );
};

struct GeometryBufferPersistentData : public RefCounted<> {

    GeometryBufferPersistentData();
    ~GeometryBufferPersistentData() override;

    // If the buffers need to be reset to initial state (possibly upon first start or buffer
    // reallocation)
    bool need_reset_ {true};

    TRef<RDGTexture> prev_G_depth_;
    TRef<RDGTexture> prev_G_normal_;
    TRef<RDGTexture> prev_G_geometry_normal_;
    TRef<RDGTexture> prev_G_transmittance_;
    TRef<RDGTexture> prev_G_motion_vector_;

    bool MakeSureExists(
        RendererView * view, RenderGraphBuilder & builder
    );

    // Called at the end of the frame to update persistent data
    void FinalUpdate (
        RendererView * view
    );
};

MI_NAMESPACE_END
#endif //MI_R_GEOMETRY_BUFFER_H