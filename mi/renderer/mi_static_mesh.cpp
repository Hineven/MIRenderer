/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_static_mesh.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_view.h"

MI_NAMESPACE_BEGIN
    StaticMesh::StaticMesh(uint32_t index, World * world): Renderable(RenderableType::kStaticMesh, index, world) {}

StaticMesh::~StaticMesh() {}

TRef<StaticMesh> StaticMesh::Create(World *world, Transform transform) {
    auto index = AllocateRenderableIndexFromWorld(world);
    if (index == UINT32_MAX) {
        MI_LOG(MIInfraLogType::kError, "Failed to allocate static mesh index from world.");
        return nullptr;
    }
    auto mesh = TRef(new StaticMesh(index, world));
    mesh->SetTransform(transform);
    mesh->index_ = index;
    mesh->world_ = world;
}


void StaticMesh::AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) {
    geometries_.push_back(geom);
    materials_.push_back(mat);
    dirty_ = true;
}

void StaticMesh::Update (RendererView * view, RenderGraphBuilder & builder) {
    if (!dirty_) return;

    auto device_world = view->world_->GetDevice();
    // Used to index the material indices buffer for geometries within the renderable.
    auto offset = view->static_mesh_geometry_material_index_top;
    view->static_mesh_geometry_material_index_top += uint32_t(geometries_.size());
    renderable_header_.NumGeometries = uint32_t(geometries_.size());asdasdas
}

RenderableHeader StaticMesh::GetDeviceRenderableHeader() const {
    return ReinterpretAs<RenderableHeader>(renderable_header_);
}


MI_NAMESPACE_END