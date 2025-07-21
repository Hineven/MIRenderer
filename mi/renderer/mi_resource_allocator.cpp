/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_resource_allocator.h"
#include "core/infra.h"

#include <renderer/mi_material.h>
#include <rhi/rhi.h>
#include "rhi/rhi_buffer.h"
#include "rhi/rhi_bindless.h"
#include "shaders/shared/SharedMaterial.hlsl"


MI_NAMESPACE_BEGIN

DeviceBindlessResourceAllocator::DeviceBindlessResourceAllocator():
material_slots_(kMaxNumMaterials), geometry_slots_(kMaxNumGeometries), static_mesh_slots_(kMaxNumStaticMeshes), volume_primitives_slots_(kMaxNumVolumePrimitives) {
    vertex_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kVertex | RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput,
        128
    );
    vertex_uber_buffer_->SetName("VertexUberBuffer");
    index_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kIndex | RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput,
        128
    );
    index_uber_buffer_->SetName("IndexUberBuffer");
    material_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(MaterialHeader) * kMaxNumMaterials, RHIBufferUsageFlagBits::kStorage}
    );
    material_header_buffer_->SetName("MaterialHeaderBuffer");
    geometry_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(GeometryHeader) * kMaxNumGeometries, RHIBufferUsageFlagBits::kStorage}
    );
    geometry_header_buffer_->SetName("GeometryHeaderBuffer");
    static_mesh_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(StaticMeshHeader) * kMaxNumStaticMeshes, RHIBufferUsageFlagBits::kStorage}
    );
    static_mesh_header_buffer_->SetName("StaticMeshHeaderBuffer");
    static_mesh_description_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        1, sizeof(uint2) * kMaxNumStaticMeshGeometryMaterialPairs
    );
}

DeviceBindlessResourceAllocator::~DeviceBindlessResourceAllocator() {

}



MI_NAMESPACE_END