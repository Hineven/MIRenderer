/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_geometry.h"

#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_geometry.h>
#include <rhi/rhi.h>

#include "rdg/rdg_helper.h"
#include "renderer/mi_scene.h"
#include "rhi/rhi_as.h"
MI_NAMESPACE_BEGIN
DeviceGeometry::DeviceGeometry(DeviceBindlessResourceAllocator * allocator) {
    allocator_ = allocator;
    index_ = allocator_->AllocateGeometrySlot();
}

DeviceGeometry::~DeviceGeometry() {
    if (IsValid()) allocator_->FreeGeometrySlot(index_);
}

Geometry::Geometry() {

}

Geometry::~Geometry() {
    ReleaseHost();
    ReleaseDevice();
}


TRef<Geometry> Geometry::CreateFromVertices(std::span<DefaultStaticMeshVertex> vertices, std::span<uint32_t> indices) {
    auto geom = TRef<Geometry>(new Geometry());
    if (indices.data() == nullptr) {
        // Non-indexed geometry
        assert(false && "Not implemented");
        return nullptr;
    }
    geom->vertices_.resize(vertices.size());
    geom->indices_.resize(indices.size());
    // geom->dynamic_ = dynamic;
    std::copy(vertices.begin(), vertices.end(), geom->vertices_.begin());
    std::copy(indices.begin(), indices.end(), geom->indices_.begin());
    return geom;
}

void Geometry::UpdateOnDevice_Async(DeviceBindlessResourceAllocator *alloc, RHICommandQueueGraphics & queue) {
    mi_check(GetVertexBufferSize() < UINT32_MAX, "Too large geometry! Overflowing allocation size for vertex buffer.");
    mi_check(GetIndexBufferSize() < UINT32_MAX, "Too large geometry! Overflowing allocation size for index buffer.");
    if (dirty_) {
        if (!device_geometry_) {
            device_geometry_ = TRef(new DeviceGeometry(alloc));
            mi_check(device_geometry_->IsValid(), "Failed to allocate device geometry slot. This may indicate that the device allocator is full.");
        }
        // Update geometries
        if (!device_geometry_->vertex_buffer_ || device_geometry_->vertex_buffer_->GetSize() != GetVertexBufferSize()) {
            device_geometry_->vertex_buffer_ = alloc->AllocateVertexBuffer((uint32_t)GetVertexBufferSize()).first;
        }
        if (!device_geometry_->index_buffer_ || device_geometry_->index_buffer_->GetSize() != GetIndexBufferSize()) {
            device_geometry_->index_buffer_ = alloc->AllocateIndexBuffer((uint32_t)GetIndexBufferSize()).first;
        }
        Helpers::Upload_Async(queue, device_geometry_->vertex_buffer_->GetRHI(), vertices_.data(), GetVertexBufferSize());
        Helpers::Upload_Async(queue, device_geometry_->index_buffer_->GetRHI(), indices_.data(), GetIndexBufferSize());
        // Update geometry header
        device_geometry_->first_index_ = 0;
        device_geometry_->vertex_count_ = (int)vertices_.size();
        device_geometry_->index_count_ = (int)indices_.size();
        auto index = device_geometry_->GetIndex();
        auto geometry_header = GeometryHeader {
            (uint32_t)(device_geometry_->vertex_buffer_->GetOffset() / sizeof(DefaultStaticMeshVertex)),
            (uint32_t)(device_geometry_->index_buffer_->GetOffset() / sizeof(uint32_t)) + device_geometry_->first_index_,
            device_geometry_->vertex_count_, device_geometry_->index_count_
        };
        Helpers::Upload_Async(queue, alloc->GetGeometryHeaderBuffer(), index * sizeof(GeometryHeader), geometry_header);
        SetDirty(false);
    } else {
        MI_WARN("Geometry is not dirty, no need to update on device.");
    }
}

void Geometry::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    if (dirty_) {
        UpdateOnDevice_Async(alloc, RHI::Get().GetGraphicsCommandQueue());
        RHI::Get().GetGraphicsCommandQueue().WaitForIdle("Geometry::UpdateOnDevice");
    }
}

void Geometry::ReleaseHost() {
    vertices_.clear();
    indices_.clear();
}

void Geometry::ReleaseDevice() {
    device_geometry_.SafeRelease();
}

void Geometry::SetName([[maybe_unused]] std::string_view name) {
    // TODO
}


MI_NAMESPACE_END