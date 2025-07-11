/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_STATIC_MESH_H
#define MI_STATIC_MESH_H

#include <span>
#include <vector>

#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

class StaticMesh : public Renderable {
public:

    void AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) ;
    void ClearMeshPrimitives ();
    void Update (RendererView * view, RenderGraphBuilder & builder);

    FORCEINLINE const std::vector<TRef<Geometry>> & GetGeometries () const { return geometries_; }
    FORCEINLINE const std::vector<TRef<Material>> & GetMaterials () const { return materials_; }

    static TRef<StaticMesh> Create (RendererScene * world, Transform transform = {}) ;

    RenderableHeader GetDeviceRenderableHeader() const override;

    FORCEINLINE bool IsDynamic () const {
        return dynamic_;
    }

    FORCEINLINE void SetDynamic (bool dynamic) {
        dynamic_ = dynamic;
        SetDirty(true);
    }

    FORCEINLINE bool IsRayTraced () const {
        return is_ray_traced_;
    }

    FORCEINLINE void SetRayTraced (bool ray_traced) {
        is_ray_traced_ = ray_traced;
        SetDirty(true);
    }

    FORCEINLINE RHIAccelerationStructure * GetBLAS () const {
        return BLAS_.Raw();
    }

    constexpr static RenderableType kRenderableType = RenderableType::kStaticMesh;

protected:

    StaticMesh(uint32_t index, RendererScene * world) ;
    ~StaticMesh() override;

    std::vector<TRef<Geometry>> geometries_;
    std::vector<TRef<Material>> materials_;

    // Store a list of material indices on the device
    TRef<DeviceBufferHeapBuffer> geometry_material_indices_;
    StaticMeshRenderableHeader renderable_header_;

    // If this static mesh is ray-traced, it should have a bottom-level acceleration structure.
    TRef<RHIAccelerationStructure> BLAS_;

    // If this static mesh is ray-traced. If true, it should have an acceleration structure.
    bool is_ray_traced_ {};

    // If true, the mesh is meant to work faster with dynamic geometry updates.
    bool dynamic_ {};

};


MI_NAMESPACE_END

#endif //MI_STATIC_MESH_H
