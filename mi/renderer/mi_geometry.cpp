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
MI_NAMESPACE_BEGIN

DeviceGeometry::DeviceGeometry(CommonGroupedDeviceResourceAllocator * allocator) {
    allocator_ = allocator;
}

DeviceGeometry::~DeviceGeometry() {
    if (vertex_buffer_.buffer)
        allocator_->FreeVertexBuffer(vertex_buffer_);
    if (index_buffer_.buffer)
        allocator_->FreeIndexBuffer(index_buffer_);
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
    std::copy(vertices.begin(), vertices.end(), geom->vertices_.begin());
    std::copy(indices.begin(), indices.end(), geom->indices_.begin());
    return geom;
}

void Geometry::UpdateOnDevice_Async(CommonGroupedDeviceResourceAllocator *alloc) {
    mi_assert(!device_geometry_, "Device geometry already created.");
    mi_check(GetVertexBufferSize() < UINT32_MAX, "Too large geometry! Overflowing allocation size for vertex buffer.");
    mi_check(GetIndexBufferSize() < UINT32_MAX, "Too large geometry! Overflowing allocation size for index buffer.");
    if (dirty_) {
        if (!device_geometry_) {
            device_geometry_ = TRef(new DeviceGeometry(alloc));
        }
        if (device_geometry_->vertex_buffer_.size != GetVertexBufferSize()) {
            device_geometry_->vertex_buffer_ = alloc->AllocateVertexBuffer((uint32_t)GetVertexBufferSize());
        }
        if (device_geometry_->index_buffer_.size != GetIndexBufferSize()) {
            device_geometry_->index_buffer_ = alloc->AllocateIndexBuffer((uint32_t)GetIndexBufferSize());
        }
        Helpers::Upload_Async(device_geometry_->vertex_buffer_, vertices_.data(), GetVertexBufferSize());
        Helpers::Upload_Async(device_geometry_->index_buffer_, indices_.data(), GetIndexBufferSize());
        device_geometry_->first_index_ = 0;
        device_geometry_->vertex_count_ = (int)vertices_.size();
        device_geometry_->index_count_ = (int)indices_.size();
        dirty_ = false;
    }
}

void Geometry::UpdateOnDevice(CommonGroupedDeviceResourceAllocator * alloc) {
    if (dirty_) {
        UpdateOnDevice_Async(alloc);
        RHI::Get().GetGraphicsCommandQueue().WaitForIdle();
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