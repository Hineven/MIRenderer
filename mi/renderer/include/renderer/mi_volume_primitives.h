/*
 * Created: 2025/6/1
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VOLUME_PRIMITIVES_H
#define MI_VOLUME_PRIMITIVES_H

#include <span>
#include <vector>

#include "mi_buffer_heap.h"
#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_fwd.h"

#include "../shaders/shared/SharedVolumePrimitives.hlsl"

MI_NAMESPACE_BEGIN

class DeviceVolumePrimitives : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    friend class VolumePrimitives;
    FORCEINLINE bool IsValid () const {
        return index_ != UINT32_MAX;
    }
    FORCEINLINE uint32_t GetIndex () const {
        return index_;
    }
    FORCEINLINE uint32_t GetPrimitiveOffset () const {
        return (uint32_t)(primitive_buffer_->GetOffset() / sizeof(PackedVolumePrimitive));
    }

protected:
    DeviceVolumePrimitives (DeviceBindlessResourceAllocator * allocator) ;
    ~DeviceVolumePrimitives() ;

    uint32_t index_ {UINT32_MAX}; // Index of the volume primitives in the bindless device allocator
    // Store a list of volume primitives on the device
    TRef<DeviceUberBufferAllocation> primitive_buffer_;
};

class VolumePrimitives : public NonMovable, public NonCopyable, public RefCounted<> {
public:
    friend class Renderer;

    void UpdateOnDevice_Async (DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) ;
    void UpdateOnDevice (DeviceBindlessResourceAllocator * alloc) ;
    FORCEINLINE DeviceVolumePrimitives * GetDeviceVolumePrimitives () const {
        return device_volume_primitives_.Raw();
    }
    static TRef<VolumePrimitives> Create () ;
    void SetPrimitives (const std::vector<PackedVolumePrimitive> & primitives) ;

    FORCEINLINE bool IsDirty () const {
        return dirty_;
    }
    void SetDirty (bool dirty = true) ;

    constexpr static uint32_t kVolumePrimitiveAllocatorUberBufferIndex = 0;

    FORCEINLINE uint32_t GetNumPrimitives () const {
        return (uint32_t)primitives_.size();
    }

protected:
    // All volume primitive data are allocated in a single buffer heap with a single buffer.
    // (Registered at kVolumePrimitiveAllocatorBufferHeapIndex)
    static void SetupAllocatorUberBuffer (DeviceBindlessResourceAllocator * allocator) ;

    TRef<DeviceVolumePrimitives> device_volume_primitives_;
    std::vector<PackedVolumePrimitive> primitives_;

    bool dirty_ {true};

    DirtyTracker<VolumePrimitives> * tracker_ {};
};

class VolumePrimitivesInstance : public Renderable {
public:

    static TRef<VolumePrimitivesInstance> Create (Scene * scene, VolumePrimitives * primitives, Transform transform = {}) ;

    RenderableHeader GetDeviceRenderableHeader() const override;

    constexpr static RenderableType kRenderableType = RenderableType::kVolumePrimitivesInstance;


    FORCEINLINE VolumePrimitives * GetVolumePrimitives () const {
        return volume_primitives_.Raw();
    }

    void Update(RendererView * view, RenderGraphBuilder & builder) override ;

protected:

    VolumePrimitivesInstance(Scene * world) ;
    ~VolumePrimitivesInstance() override;

    TRef<VolumePrimitives> volume_primitives_;

};


MI_NAMESPACE_END

#endif //MI_VOLUME_PRIMITIVES_H
