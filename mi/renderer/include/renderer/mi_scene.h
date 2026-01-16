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
#include <random>
#include <core/base.h>
#include <core/refcounted.h>
#include <core/util/slot_allocator.h>
#include <rhi/rhi_fwd.h>

#include <renderer/mi_renderer_fwd.h>
#include <renderer/mi_renderable.h>
#include <renderer/mi_lights.h>
#include <renderer/mi_aabb.h>

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
    TRef<RHIBuffer> d_renderable_transforms_; // Array of float3x4 matrices (to world)
    TRef<RHIBuffer> d_renderable_inverse_transforms_; // Array of float3x4 matrices (to local)
    TRef<RHIBuffer> d_renderable_normal_transforms_;
    TRef<RHIBuffer> d_renderable_headers_;

    // Top level acceleration structure for ray-traced objects
    TRef<RHIAccelerationStructure> TLAS_;

    // Track last built TLAS instance count to decide Build vs Update.
    // Vulkan requires the number of primitives (instances) to remain the same for Update mode.
    // Initialize to an invalid value to force a Build on first use.
    uint32_t tlas_instance_count_ = UINT32_MAX;
};

// Device light structure used for light sampling. Held by the scene and organizes all lights.
class DeviceLightStructure : public NonMovable, public NonCopyable, public RefCounted<true> {
public:
    TRef<DeviceUberBufferInterface> lights_uber_buffer_; // A list of all area lights

};

class Scene : public NonMovable, public NonCopyable {
public:
    friend class Renderable;

    // Visibility buffer reserved 24 bits for renderable index.
    constexpr static uint32_t kMaxNumRenderablesMax = 1 << 24;
    // Limit to a smaller number for memory efficiency. This can be increased if needed.
    constexpr static uint32_t kMaxNumRenderables = 4096;

    static_assert(kMaxNumRenderables <= kMaxNumRenderablesMax,
        "Max number of renderables exceeds visibility buffer design limit.");

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

    FORCEINLINE const AABB & GetAABB () const {
        return aabb_;
    }

    void UpdateAABB () ;


    DirectionalLight directional_light_{};

protected:

    AABB aabb_;

    TRef<Texture> sky_cube_;

    std::vector<TRef<Renderable>> renderables_;
    std::mt19937 renderable_hash_generator {12345};

    uint32_t AllocateRenderableIndexAndHash (Renderable * renderable) ;
    FORCEINLINE void FreeRenderabeIndex (uint32_t index) {
        renderable_slots_.FreeSlot(index);
    }

    SlotAllocator renderable_slots_;

    TRef<DeviceScene> device_scene_;
};

MI_NAMESPACE_END

#endif //MI_WORLD_H
