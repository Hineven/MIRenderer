/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_geometry.h"

#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_geometry.h>
#include <renderer/mi_helpers.h>
#include <rhi/rhi.h>

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

void Geometry::CreateOnDevice(CommonGroupedDeviceResourceAllocator *alloc) {
    mi_assert(!device_geometry_, "Device geometry already created.");
    auto device = TRef(new DeviceGeometry(alloc));
    mi_check(GetVertexBufferSize() < UINT32_MAX, "Too large geometry! Overflowing allocation size for vertex buffer.");
    mi_check(GetIndexBufferSize() < UINT32_MAX, "Too large geometry! Overflowing allocation size for index buffer.");
    auto vbuf = alloc->AllocateVertexBuffer((uint32_t)GetVertexBufferSize());
    auto ibuf = alloc->AllocateIndexBuffer((uint32_t)GetIndexBufferSize());
    device->vertex_buffer_ = vbuf;
    device->index_buffer_ = ibuf;
    device->first_index_ = 0;
    device->vertex_count_ = (int)vertices_.size();
    device->index_count_ = (int)indices_.size();
    device_geometry_ = std::move(device);
    dirty_ = true;
}

void Geometry::SyncAndUpdateOnDevice () {
    mi_assert(device_geometry_, "Device geometry not created.");
    Helpers::Upload(device_geometry_->vertex_buffer_, vertices_.data(), GetVertexBufferSize());
    Helpers::Upload(device_geometry_->index_buffer_, indices_.data(), GetIndexBufferSize());
    RHI::Get().GetGraphicsCommandQueue().EnqueueTranslateAndSubmit();
    RHI::Get().WaitForIdle();
    device_geometry_->first_index_ = 0;
    dirty_ = false;
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