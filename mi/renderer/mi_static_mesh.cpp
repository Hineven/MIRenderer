/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"

MI_NAMESPACE_BEGIN

StaticMesh::StaticMesh(): Renderable(RenderableType::kStaticMesh) {}

StaticMesh::~StaticMesh() {}

void StaticMesh::AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) {
    geometries_.push_back(geom);
    materials_.push_back(mat);
    dirty_ = true;
}

void StaticMesh::Update (RenderGraphBuilder & builder) {
    // TODO update device static mesh header
}

MI_NAMESPACE_END