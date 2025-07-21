/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "renderer/mi_static_mesh.h"

#include <gtest/internal/gtest-port.h>

#include "rdg/rdg_builder.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_buffer_heap.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_view.h"
#include "renderer/mi_resource_allocator.h"
#include "rhi/rhi_as.h"

MI_NAMESPACE_BEGIN

DeviceStaticMesh::DeviceStaticMesh(DeviceBindlessResourceAllocator * in_allocator) {
    allocator_ = in_allocator;
    index_ = allocator_->AllocateStaticMeshSlot();
}

DeviceStaticMesh::~DeviceStaticMesh() {
    if (IsValid()) allocator_->FreeStaticMeshSlot(index_);
}

void StaticMesh::AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) {
    assert(mat->GetDeviceMaterial() && "Material must have a device material. Call UpdateOnDevice() on the material first.");
    geometries_.push_back(geom);
    materials_.push_back(mat);
    SetDirty(true);
}

void StaticMesh::ClearMeshPrimitives() {
    geometries_.clear();
    materials_.clear();
    device_static_mesh_ = {};
    SetDirty(true);
}

void StaticMesh::UpdateOnDevice_Async (DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    if (!IsDirty()) return;
    if (IsEmpty()) return;
    if (!device_static_mesh_) {
        device_static_mesh_ = new DeviceStaticMesh(alloc);
        mi_check(device_static_mesh_.IsValid(), "Failed to allocate static mesh slot. Maybe too many static meshes?");
    }
    // Update geometry - material pairs
    auto desired_num_pairs = (uint32_t)geometries_.size();
    if (!device_static_mesh_->geometry_material_indices_
        || device_static_mesh_->geometry_material_indices_->GetRHI().size / (sizeof(uint32_t) * 2) != desired_num_pairs) {
        device_static_mesh_->geometry_material_indices_ = alloc->GetStaticMeshDescriptionUberBuffer()->AllocateRefCounted(
            desired_num_pairs * sizeof(uint32_t) * 2
        ).first;
    }
    std::vector<uint32_t> data;
    data.reserve(geometries_.size() * 2);
    for (int i = 0; i < (int)geometries_.size(); i++) {
        auto geom = geometries_[i];
        auto mat = materials_[i];
        data.emplace_back(geom->GetDeviceGeometry()->GetIndex());
        data.emplace_back(mat->GetDeviceMaterial()->GetIndex());
    }
    Helpers::Upload_Async(queue,
        device_static_mesh_->geometry_material_indices_->GetRHI(), data.data(), sizeof(uint32_t) * data.size()
    );
    // Update header
    auto header = StaticMeshHeader {
        (uint32_t)(device_static_mesh_->geometry_material_indices_->GetRHI().offset / (2 * sizeof(uint32_t))),
        (uint32_t)geometries_.size(),
    };
    Helpers::Upload_Async(queue,
        alloc->GetStaticMeshHeaderBuffer(),
        sizeof(StaticMeshHeader) * device_static_mesh_->index_,
        header
    );
    // Update BLAS if needed
    if (!IsRayTraced()) {
        device_static_mesh_->BLAS_ = {};
    } else {
        if (geometries_.empty()) {
            // No geometries, release BLAS. NullDescriptorSet feature will take care of this case.
            device_static_mesh_->BLAS_ = {};
        } else {
            // Update acceleration structure for raytracing
            auto geometries = queue.Allocate<RHIASGeometry[]>(geometries_.size());
            RHIAccelerationStructureBuildFlags build_flags = RHIAccelerationStructureBuildFlagBits::kPreferFastTrace;
            build_flags = build_flags | (dynamic_ ? RHIAccelerationStructureBuildFlagBits::kAllowUpdate : RHIAccelerationStructureBuildFlagBits::kNone);
            for (auto [i, geometry] : std::views::enumerate(geometries_)) {
                auto material = materials_[i];
                RHIASGeometryFlags geometry_flags = material->IsOpaque() ? RHIASGeometryFlagBits::kOpaque : RHIASGeometryFlagBits::kNone;
                auto device_geom = geometry->GetDeviceGeometry();
                geometries[i] = RHIASGeometry{
                    RHIASGeometryType::kTriangles,
                    geometry_flags,
                    {
                        device_geom->GetDeviceVertexBuffer()->GetRHI(),
                        sizeof(DefaultStaticMeshVertex),
                        (uint32_t)device_geom->GetVertexCount(),
                        RHIVertexAttributeFormatType::k3xFp32,
                        RHIBufferSpan{
                            device_geom->GetDeviceIndexBuffer()->GetRHI().buffer,
                            device_geom->GetDeviceIndexBuffer()->GetRHI().offset + sizeof(uint32_t) * device_geom->GetDeviceFirstIndex(),
                            device_geom->GetDeviceIndexBuffer()->GetRHI().size
                        },
                        (uint32_t)device_geom->GetIndexCount(),
                        RHIIndexType::kUint32
                    }
                };
            }
            if (!device_static_mesh_->BLAS_) {
                // Create
                device_static_mesh_->BLAS_ = RHI::Get().CreateAccelerationStructure(
                    RHIAccelerationStructureType::kBottomLevel
                );
            }
            auto build_info = RHIAccelerationStructureBuildGeometryInfo{
                RHIAccelerationStructureType::kBottomLevel,
                build_flags,
                RHIAccelerationStructureBuildMode::kUpdate,
                device_static_mesh_->BLAS_.Raw(),
                device_static_mesh_->BLAS_.Raw(),
                {geometries, geometries_.size()}, {}, {}
            };
            auto sizes = device_static_mesh_->BLAS_->GetBuildSizes(build_info);
            bool updated = false;
            // Try to update
            if (dynamic_ && sizes.acceleration_structure_size <= device_static_mesh_->BLAS_->GetSize()) {
                auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());
                updated = true;
            }
            if (!updated) {
                // Need rebuild
                // Re-create
                device_static_mesh_->BLAS_->Create(sizes.acceleration_structure_size);
                // Build
                build_info.mode = RHIAccelerationStructureBuildMode::kBuild;
                build_info.src_acceleration_structure = {};
                build_info.dst_acceleration_structure = device_static_mesh_->BLAS_.Raw();
                auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());
            }
            // Barrier
            queue.AccelerationStructureBarrier(
                device_static_mesh_->BLAS_.Raw(),
                RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIPipelineStageFlagBits::kRayTracing,
                RHIGPUAccessFlagBits::kAccelerationStructureWrite,
                RHIGPUAccessFlagBits::kAccelerationStructureRead
            );
        }
    }
    SetDirty(false);
}

void StaticMesh::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    UpdateOnDevice_Async(alloc, queue);
    queue.WaitForIdle("StaticMesh::UpdateOnDevice");
}

TRef<StaticMesh> StaticMesh::Create(bool is_ray_traced, bool dynamic) {
    auto mesh = TRef(new StaticMesh());
    mesh->is_ray_traced_ = is_ray_traced;
    mesh->dynamic_ = dynamic;
    return mesh;
}


StaticMeshInstance::StaticMeshInstance(Scene * scene): Renderable(RenderableType::kStaticMeshInstance, scene) {}

StaticMeshInstance::~StaticMeshInstance() {}

void StaticMeshInstance::Update([[maybe_unused]] RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    SetDirty(false);
}


TRef<StaticMeshInstance> StaticMeshInstance::Create(Scene *scene, StaticMesh * static_mesh, Transform transform) {
    auto mesh = TRef(new StaticMeshInstance(scene));
    if (mesh->IsValid()) {
        mesh->SetTransform(transform);
        mesh->scene_ = scene;
        mesh->static_mesh_ = static_mesh;;

        return std::move(mesh);
    }
    return {};
}
RenderableHeader StaticMeshInstance::GetDeviceRenderableHeader() const {
    return  {};
}


MI_NAMESPACE_END