/*
 * Created: 2025/6/1
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_volume_primitives.h>

#include "rdg/rdg_builder.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_buffer_heap.h"
#include "renderer/mi_resource_allocator.h"

MI_NAMESPACE_BEGIN

// DeviceVolumePrimitives implementation
DeviceVolumePrimitives::DeviceVolumePrimitives(DeviceBindlessResourceAllocator * allocator) {
    index_ = allocator->AllocateVolumePrimitivesSlot();
}

DeviceVolumePrimitives::~DeviceVolumePrimitives() {
    // Note: We don't have access to the allocator here, so we assume it will be cleaned up elsewhere
    // This follows the same pattern as DeviceStaticMesh in the reference implementation
}

// VolumePrimitives implementation
TRef<VolumePrimitives> VolumePrimitives::Create() {
    auto volume_primitives = TRef(new VolumePrimitives());
    return volume_primitives;
}

void VolumePrimitives::SetDirty(bool dirty) {
    dirty_ = dirty;
    if (tracker_) {
        tracker_->OnObjectTurnedDirty(this);
    }
}

void VolumePrimitives::SetPrimitives(const std::vector<VolumePrimitive> & primitives) {
    primitives_ = primitives;
    SetDirty();
}

void VolumePrimitives::SetupAllocatorUberBuffer(DeviceBindlessResourceAllocator * allocator) {
    // Setup the uber buffer for volume primitives similar to static mesh implementation
    auto uber_buffer = allocator->GetCustomUberBuffer(kVolumePrimitiveAllocatorUberBufferIndex);
    if (!uber_buffer) {
        // Create a new uber buffer for volume primitives if it doesn't exist
        allocator->RegisterCustomUberBuffer(
            kVolumePrimitiveAllocatorUberBufferIndex,
            DefaultDeviceUberBuffer::Create(RHIBufferUsageFlagBits::kStorage, 16).Raw()
        );
    }
}

void VolumePrimitives::UpdateOnDevice_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    if (!dirty_) return;
    if (primitives_.empty()) return;

    if (!device_volume_primitives_) {
        device_volume_primitives_ = new DeviceVolumePrimitives(alloc);
        mi_check(device_volume_primitives_->IsValid(), "Failed to allocate volume primitives slot.");
    }

    // Calculate required buffer size
    auto required_size = primitives_.size() * sizeof(VolumePrimitive);

    // Allocate or reallocate buffer if needed
    if (!device_volume_primitives_->primitive_buffer_ ||
        device_volume_primitives_->primitive_buffer_->GetRHI().size < required_size) {
        device_volume_primitives_->primitive_buffer_ = alloc->GetCustomUberBuffer(kVolumePrimitiveAllocatorUberBufferIndex)
            ->AllocateRefCounted(required_size).first;
    }

    // Upload primitive data
    Helpers::Upload_Async(queue,
        device_volume_primitives_->primitive_buffer_->GetRHI(),
        primitives_.data(),
        required_size
    );

    VolumePrimitivesHeader header = {
        (uint32_t)primitives_.size(),
        (uint32_t)(device_volume_primitives_->primitive_buffer_->GetRHI().offset / sizeof(VolumePrimitive)),
    };

    // Upload header to device
    Helpers::Upload_Async(queue,
        alloc->GetCustomUberBuffer(kVolumePrimitiveAllocatorUberBufferIndex)->GetRHI(),
        sizeof(VolumePrimitivesHeader) * device_volume_primitives_->index_,
        header
    );

    dirty_ = false;
}

void VolumePrimitives::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    UpdateOnDevice_Async(alloc, queue);
    queue.WaitForIdle("VolumePrimitives::UpdateOnDevice");
}

// VolumePrimitivesInstance implementation
VolumePrimitivesInstance::VolumePrimitivesInstance(Scene * scene)
    : Renderable(RenderableType::kVolumePrimitivesInstance, scene) {
}

VolumePrimitivesInstance::~VolumePrimitivesInstance() {
}

TRef<VolumePrimitivesInstance> VolumePrimitivesInstance::Create(Scene * scene, VolumePrimitives * primitives, Transform transform) {
    auto instance = TRef(new VolumePrimitivesInstance(scene));
    if (instance->IsValid()) {
        instance->SetTransform(transform);
        instance->scene_ = scene;
        instance->volume_primitives_ = primitives;
        return std::move(instance);
    }
    return {};
}

void VolumePrimitivesInstance::Update([[maybe_unused]] RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    SetDirty(false);
}

RenderableHeader VolumePrimitivesInstance::GetDeviceRenderableHeader() const {
    return {};
}

MI_NAMESPACE_END