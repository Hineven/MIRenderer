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
#include "shaders/SharedMaterial.hlsl"


MI_NAMESPACE_BEGIN

GroupedRenderResourceAllocator::GroupedRenderResourceAllocator(DeviceBufferHeapInterface *vertex_buffer_heap, DeviceBufferHeapInterface *index_buffer_heap) {
    vertex_buffer_heap_ = vertex_buffer_heap;
    index_buffer_heap_ = index_buffer_heap;

    for (uint32_t i = kMaxNumMaterials; i > 0; i--) {
        free_material_slots_.push(i - 1);
    }
    material_header_buffer_ = RHI::Get().CreateBuffer({sizeof(MaterialHeader) * kMaxNumMaterials, RHIBufferUsageFlagBits::kUniform});
}



MI_NAMESPACE_END