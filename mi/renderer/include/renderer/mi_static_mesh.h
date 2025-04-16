/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_STATIC_MESH_H
#define MI_STATIC_MESH_H

#include <memory>
#include <span>
#include <vector>

#include "mi_world.h"
#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

class StaticMesh : public Renderable {
public:
    StaticMesh() ;
    ~StaticMesh() override;
    void AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) ;
    void Update(RenderGraphBuilder &builder) override;

    FORCEINLINE const std::vector<TRef<Geometry>> & GetGeometries () const { return geometries_; }
    FORCEINLINE const std::vector<TRef<Material>> & GetMaterials () const { return materials_; }
protected:
    std::vector<TRef<Geometry>> geometries_;
    std::vector<TRef<Material>> materials_;
};


MI_NAMESPACE_END

#endif //MI_STATIC_MESH_H
