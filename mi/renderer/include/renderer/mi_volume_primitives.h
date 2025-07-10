/*
 * Created: 2025/6/1
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VOLUME_PRIMITIVES_H
#define MI_VOLUME_PRIMITIVES_H

#include <span>
#include <vector>

#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

// 32 bytes per primitive
struct VolumePrimitive {
    glm::vec3 position;
    uint32_t packed_rotation;
    glm::vec3 scale;
    uint32_t packed_color_opacity; // RGBA color, packed into uint32_t
};

class VolumePrimitives : public Renderable {
public:

    void Update (RendererView * view, RenderGraphBuilder & builder);
    void SetPrimitives (const std::vector<VolumePrimitive> & primitives) ;
    FORCEINLINE bool IsDirty () const {return dirty_;}

    static TRef<VolumePrimitives> Create (RendererScene * world, Transform transform = {}) ;

    RenderableHeader GetDeviceRenderableHeader() const override;

    constexpr static uint32_t kVolumePrimitiveAllocatorBufferHeapIndex = 0;

    // All volume primitive data are allocated in a single buffer heap with a single buffer.
    // (Registered at kVolumePrimitiveAllocatorBufferHeapIndex)
    static void SetupAllocatorBufferHeap (CommonGroupedDeviceResourceAllocator * allocator) ;

protected:

    VolumePrimitives(uint32_t index, RendererScene * world) ;
    ~VolumePrimitives() override;

    bool dirty_ {true};

    std::vector<VolumePrimitive> primitives_;
    TRef<DeviceBufferHeapBuffer> device_primitives_;

    VolumePrimitivesRenderableHeader renderable_header_;
};


MI_NAMESPACE_END

#endif //MI_VOLUME_PRIMITIVES_H
