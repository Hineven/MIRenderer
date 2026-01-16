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
#include "shaders/shared/SharedVolumePrimitives.hlsl"
#include "shaders/shared/SharedGaussianRadianceField.hlsl"
#include "shaders/shared/SharedVolumeGrid.hlsl"


MI_NAMESPACE_BEGIN

DeviceBindlessResourceAllocator::DeviceBindlessResourceAllocator():
material_slots_(kMaxNumMaterials),
geometry_slots_(kMaxNumGeometries),
static_mesh_slots_(kMaxNumStaticMeshes),
volume_primitives_slots_(kMaxNumVolumePrimitiveGroups),
gaussian_radiance_field_slots_(kMaxNumGaussianRadianceFields),
volume_grid_slots_(kMaxNumVolumeGrids){

    vertex_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kVertex | RHIBufferUsageFlagBits::kStorage
        | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput | RHIBufferUsageFlagBits::kShaderDeviceAddress,
        128
    );
    vertex_uber_buffer_->SetName("VertexUberBuffer");

    index_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kIndex | RHIBufferUsageFlagBits::kStorage
        | RHIBufferUsageFlagBits::kAccelerationStructureBuildInput | RHIBufferUsageFlagBits::kShaderDeviceAddress,
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
        1, 16 * 1024
    );
    static_mesh_description_uber_buffer_->SetName("StaticMeshDescriptionUberBuffer");

    area_lights_uber_buffer_ = DefaultDeviceUberBuffer::Create(
        RHIBufferUsageFlagBits::kStorage,
        1, 16 * 1024
    );
    area_lights_uber_buffer_->SetName("AreaLightsUberBuffer");

    volume_primitives_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(VolumePrimitivesHeader) * kMaxNumVolumePrimitiveGroups, RHIBufferUsageFlagBits::kStorage}
    );
    volume_primitives_header_buffer_->SetName("VolumePrimitivesHeaderBuffer");

    gaussian_radiance_field_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(GaussianRadianceFieldHeader) * kMaxNumGaussianRadianceFields, RHIBufferUsageFlagBits::kStorage}
    );
    gaussian_radiance_field_header_buffer_->SetName("GaussianRadianceFieldHeaderBuffer");

    volume_grid_header_buffer_ = RHI::Get().CreateBuffer(
        {sizeof(VolumeGridHeader) * kMaxNumVolumeGrids, RHIBufferUsageFlagBits::kStorage}
    );
    volume_grid_header_buffer_->SetName("VolumeGridHeaderBuffer");
}

DeviceBindlessResourceAllocator::~DeviceBindlessResourceAllocator() {

}

size_t DeviceBindlessResourceAllocator::GetTotalAllocatedDeviceSize() const {
    size_t sum = 0;
    sum += vertex_uber_buffer_->GetRHI()->GetBufferSize();
    sum += index_uber_buffer_->GetRHI()->GetBufferSize();
    sum += static_mesh_description_uber_buffer_->GetRHI()->GetBufferSize();
    sum += area_lights_uber_buffer_->GetRHI()->GetBufferSize();
    sum += material_header_buffer_->GetBufferSize();
    sum += geometry_header_buffer_->GetBufferSize();
    sum += static_mesh_header_buffer_->GetBufferSize();
    sum += volume_primitives_header_buffer_->GetBufferSize();
    sum += gaussian_radiance_field_header_buffer_->GetBufferSize();
    return sum;
}

MI_NAMESPACE_END
