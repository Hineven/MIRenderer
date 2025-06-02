/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_static_mesh.h"

#include "rdg/rdg_builder.h"
#include "renderer/mi_buffer_heap.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_view.h"

MI_NAMESPACE_BEGIN

StaticMesh::StaticMesh(uint32_t index, RendererScene * world): Renderable(RenderableType::kStaticMesh, index, world) {}

StaticMesh::~StaticMesh() {}

TRef<StaticMesh> StaticMesh::Create(RendererScene *world, Transform transform) {
    auto index = AllocateRenderableIndexFromWorld(world);
    if (index == UINT32_MAX) {
        MI_LOG(MIInfraLogType::kError, "Failed to allocate static mesh index from world.");
        return nullptr;
    }
    auto mesh = TRef(new StaticMesh(index, world));
    mesh->SetTransform(transform);
    mesh->index_ = index;
    mesh->world_ = world;

    mesh->RegisterToWorld();

    return std::move(mesh);
}


void StaticMesh::AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) {
    assert(mat->GetDeviceMaterial());
    geometries_.push_back(geom);
    materials_.push_back(mat);
    dirty_ = true;
}

void StaticMesh::Update (RendererView * view, [[maybe_unused]] RenderGraphBuilder & builder) {
    if (!dirty_) return;
    if (geometries_.empty()) return ;
    uint32_t current_count = (uint32_t)(geometry_material_indices_ ? geometry_material_indices_->GetRHI().size : 0);
    if (geometries_.size() > current_count) {
        current_count = std::max(current_count * 2u, 4u);
        geometry_material_indices_.SafeRelease();
        geometry_material_indices_ = world_->d_static_mesh_renderable_materials_->AllocateRefCounted(current_count * sizeof(uint32_t));
    }
    auto mem = (uint32_t*)view->temp_allocator_.Allocate(geometries_.size() * sizeof(uint32_t));
    for (int i = 0; i < (int)geometries_.size(); i++) {
        mem[i] = materials_[i]->GetDeviceMaterial()->GetIndex();
    }
    view->upload_context_.AddUnsafe(geometry_material_indices_->GetRHI(), mem, geometries_.size() * sizeof(uint32_t));
    view->upload_context_.AddExtraBarrier(
        builder.Import(view->world_->d_static_mesh_renderable_materials_->GetHeapBufferBlock(0))
    );
}

RenderableHeader StaticMesh::GetDeviceRenderableHeader() const {
    return ReinterpretAs<RenderableHeader>(renderable_header_);
}


MI_NAMESPACE_END