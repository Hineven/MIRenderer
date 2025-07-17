/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "renderer/mi_static_mesh.h"

#include "rdg/rdg_builder.h"
#include "renderer/mi_buffer_heap.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_view.h"
#include "rhi/rhi_as.h"

MI_NAMESPACE_BEGIN
StaticMeshInstance::StaticMeshInstance(uint32_t index, RendererScene * world): Renderable(RenderableType::kStaticMesh, index, world) {}

StaticMeshInstance::~StaticMeshInstance() {}

TRef<StaticMeshInstance> StaticMeshInstance::Create(RendererScene *world, Transform transform) {
    auto index = AllocateRenderableIndexFromWorld(world);
    if (index == UINT32_MAX) {
        MI_LOG(MIInfraLogType::kError, "Failed to allocate static mesh index from world.");
        return nullptr;
    }
    auto mesh = TRef(new StaticMeshInstance(index, world));
    mesh->SetTransform(transform);
    mesh->index_ = index;
    mesh->world_ = world;

    mesh->RegisterToWorld();

    return std::move(mesh);
}


void StaticMeshInstance::AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) {
    assert(mat->GetDeviceMaterial() && "Material must have a device material. Call UpdateOnDevice() on the material first.");
    geometries_.push_back(geom);
    materials_.push_back(mat);
    SetDirty(true);
}

void StaticMeshInstance::ClearMeshPrimitives() {
    geometries_.clear();
    materials_.clear();
    BLAS_ = {};
    SetDirty(true);
}


void StaticMeshInstance::Update (RendererView * view, [[maybe_unused]] RenderGraphBuilder & builder) {
    if (!IsDirty()) return;
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
    if (IsRayTraced()) {
        // Update acceleration structure for raytracing
        auto & queue = RHI::Get().GetGraphicsCommandQueue();
        auto geometries = queue.Allocate<RHIASGeometry>(geometries_.size());
        RHIAccelerationStructureBuildFlags build_flags = RHIAccelerationStructureBuildFlagBits::kPreferFastTrace;
        build_flags = build_flags | (dynamic_ ? RHIAccelerationStructureBuildFlagBits::kAllowUpdate : 0);
        for (auto [i, geometry] : std::views::enumerate(geometries_)) {
            RHIASGeometryFlags geometry_flags = geometry->IsOpaque() ? RHIASGeometryFlagBits::kOpaque : 0;
            auto device_geom = geometry->device_geometry_;
            geometries[i] = RHIASGeometry{
                RHIASGeometryType::kTriangles,
                geometry_flags,
                {
                    device_geom->vertex_buffer_,
                    sizeof(DefaultStaticMeshVertex),
                    (uint32_t)device_geom->vertex_count_,
                    RHIVertexAttributeFormatType::k3xFp32,
                    RHIBufferSpan{
                        device_geom->index_buffer_.buffer,
                        device_geom->index_buffer_.offset + sizeof(uint32_t) * device_geom->first_index_,
                        device_geom->index_buffer_.size
                    },
                    (uint32_t)device_geom->index_count_,
                    RHIIndexType::kUint32
                }
            };
        }
        auto build_info = RHIAccelerationStructureBuildGeometryInfo{
            RHIAccelerationStructureType::kBottomLevel,
            build_flags,
            RHIAccelerationStructureBuildMode::kUpdate,
            BLAS_.Raw(),
            BLAS_.Raw(),
            {geometries, geometries_.size()}, {}, {}
        };
        auto sizes = BLAS_->GetBuildSizes(build_info);
        bool updated = false;
        if (BLAS_) {
            // Try to update
            if (dynamic_ && sizes.acceleration_structure_size <= BLAS_->GetSize()) {
                auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());
                updated = true;
            } // Need rebuild
        }
        if (!updated) {
            if (!BLAS_) {
                // Create
                BLAS_ = RHI::Get().CreateAccelerationStructure(
                    RHIAccelerationStructureType::kBottomLevel
                );
            }
            // Re-create
            BLAS_->Create(sizes.acceleration_structure_size);
            // Build
            build_info.mode = RHIAccelerationStructureBuildMode::kBuild;
            build_info.src_acceleration_structure = {};
            build_info.dst_acceleration_structure = BLAS_.Raw();
            auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
            queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());
        }
        // Barrier
        queue.AccelerationStructureBarrier(
            BLAS_.Raw(),
            RHIPipelineStageFlagBits::kAccelerationStructureBuild,
            RHIPipelineStageFlagBits::kRayTracing,
            RHIGPUAccessFlagBits::kShaderWrite,
            RHIGPUAccessFlagBits::kAccelerationStructureRead
        );
    }

    SetDirty(false);
}

RenderableHeader StaticMeshInstance::GetDeviceRenderableHeader() const {
    return ReinterpretAs<RenderableHeader>(renderable_header_);
}


MI_NAMESPACE_END