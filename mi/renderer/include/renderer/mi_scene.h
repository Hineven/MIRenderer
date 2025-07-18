/*
 * Created: 2025/4/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_WORLD_H
#define MI_WORLD_H

#include <set>
#include <stack>
#include <vector>
#include <core/base.h>
#include <core/refcounted.h>
#include <core/util/slot_allocator.h>
#include <rhi/rhi_fwd.h>

#include <renderer/mi_renderer_fwd.h>
#include <renderer/mi_renderable.h>
MI_NAMESPACE_BEGIN

// Integrated class managing the rendering world. This class is not for general use and should only be used
// for rendering. Scene management is not its responsibility.
// It is responsible for holding renderables and rendering resources of a scene.
class DeviceScene : public NonCopyable, public NonMovable, public RefCounted<true> {
public:
    friend class Renderable;
    friend struct RendererView;
    friend class Renderer;
    friend class Scene;

protected:

    DeviceScene();
    ~DeviceScene();

    // Indexed with renderable index.
    TRef<RHIBuffer> d_renderable_transforms_;
    TRef<RHIBuffer> d_renderable_normal_transforms_;
    TRef<RHIBuffer> d_renderable_headers_;

    // Top level acceleration structure for ray-traced objects
    TRef<RHIAccelerationStructure> TLAS_;
};

class Scene : public NonMovable, public NonCopyable {
public:
    friend class Renderable;

    constexpr static uint32_t kMaxNumRenderables = 4096;

    Scene();
    ~Scene();

    void RemoveRenderable (Renderable * renderable) ;

    FORCEINLINE const std::vector<TRef<Renderable>> & GetRenderables () const {
        return renderables_;
    }
    void SetSkyCube (Texture * texture) ;

    FORCEINLINE Texture * GetSkyTexture () const {
        return sky_cube_.Raw();
    }

    FORCEINLINE DeviceScene * GetDeviceScene () const {
        return device_scene_.Raw();
    }

    // Create the scene on the device.
    // Unlike geometry & material, device static mesh & device scene are manually
    // updated in the renderer. See renderer implementation for details.
    void CreateOnDevice () ;

protected:

    TRef<Texture> sky_cube_;

    std::vector<TRef<Renderable>> renderables_;

    FORCEINLINE uint32_t AllocateRenderableIndex () {
        return renderable_slots_.AllocateSlot();
    }
    FORCEINLINE void FreeRenderabeIndex (uint32_t index) {
        renderable_slots_.FreeSlot(index);
    }

    SlotAllocator renderable_slots_;

    TRef<DeviceScene> device_scene_;
};

MI_NAMESPACE_END

#endif //MI_WORLD_H
