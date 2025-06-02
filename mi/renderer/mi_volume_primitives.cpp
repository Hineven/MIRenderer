/*
 * Created: 2025/6/1
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_volume_primitives.h>

#include "rdg/rdg_builder.h"
#include "renderer/mi_renderer.h"
#include "renderer/mi_resource_allocator.h"

MI_NAMESPACE_BEGIN

TRef<VolumePrimitives> VolumePrimitives::Create(RendererScene *world, Transform transform) {
    auto index = AllocateRenderableIndexFromWorld(world);
    if (index == UINT32_MAX) {
        MI_LOG(MIInfraLogType::kError, "Failed to allocate static mesh index from world.");
        return nullptr;
    }
    auto primitives = TRef(new VolumePrimitives(index, world));
    primitives->SetTransform(transform);
    primitives->index_ = index;
    primitives->world_ = world;

    primitives->RegisterToWorld();

    return std::move(primitives);
}

RenderableHeader VolumePrimitives::GetDeviceRenderableHeader() const {
    return ReinterpretAs<RenderableHeader>(renderable_header_);
}

void VolumePrimitives::SetPrimitives(const std::vector<VolumePrimitive> & primitives) {
    primitives_ = primitives;
    dirty_ = true;
}

void VolumePrimitives::Update(RendererView *view, RenderGraphBuilder &builder) {
    if (dirty_) {
        if (primitives_.empty()) {
            if (device_primitives_) device_primitives_.SafeRelease();
        } else {
            // (Re) allocate if necessary.
            auto requested_size = primitives_.size() * sizeof(VolumePrimitive);
            if (!device_primitives_
                || device_primitives_->GetRHI().size < requested_size
                || device_primitives_->GetRHI().size >= requested_size + 128u) {
                device_primitives_ = Renderer::Get().GetDeviceAllocator()->GetCustomBufferHeap(
                    kVolumePrimitiveAllocatorBufferHeapIndex
                )->AllocateRefCounted(
                    uint32_t(primitives_.size() * sizeof(VolumePrimitive))
                );
            }
            // Upload primitives to the device if reallocated or dirty.
            auto buffer = builder.Import(device_primitives_->GetRHI().buffer);
            view->upload_context_.Add(buffer, primitives_.data(), primitives_.size() * sizeof(VolumePrimitive), device_primitives_->GetRHI().offset);
        }
        dirty_ = false;
    }
}

VolumePrimitives::~VolumePrimitives() {

}

void VolumePrimitives::SetupAllocatorBufferHeap(CommonGroupedDeviceResourceAllocator *allocator) {
    auto heap =
        DefaultDeviceBufferHeap::Create(RHIBufferUsageFlagBits::kStorage, 64, 1024 * 1024 * 1024);
    heap->PreAllocateBlocks(1);
    heap->SetNumBufferBlockLimit(1);
    allocator->RegisterCustomBufferHeap(
        kVolumePrimitiveAllocatorBufferHeapIndex,
        heap.Raw()
    );
}


MI_NAMESPACE_END