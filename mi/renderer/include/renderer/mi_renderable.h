/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERABLE_H
#define MI_RENDERABLE_H

#include "mi_aabb.h"
#include "../../shaders/shared/SharedRenderable.hlsl"

#include "mi_scene.h"
#include "core/base.h"
#include "core/common.h"
#include "core/infra.h"
#include "core/refcounted.h"
#include "renderer/mi_renderer_fwd.h"
#include "renderer/mi_renderer_types.h"
#include "renderer/mi_transform.h"

MI_NAMESPACE_BEGIN

class StaticMeshInstance;
class RenderGraphBuilder;

class Renderable : public NonMovable, public NonCopyable, public RefCounted<> {
public:
    // friend class DeviceScene;
    virtual ~Renderable();
    FORCEINLINE bool IsVisible() const { return visible_; }
    FORCEINLINE void SetVisible(bool visible) { visible_ = visible; }
    // Dirty means that the renderer will make a call to Update before rendering.
    FORCEINLINE bool IsDirty () const { return dirty_; }
    FORCEINLINE void SetDirty (bool dirty = true) { dirty_ = dirty; }

    FORCEINLINE bool IsRayTraced () const { return ray_traced_; }
    FORCEINLINE void SetRayTraced (bool ray_traced) { ray_traced_ = ray_traced; SetDirty(); }

    // Override the functions if the renderable can be ray-traced.
    virtual RHIAccelerationStructure * GetBLAS () const { return nullptr; }
    constexpr static uint32_t kInvalidRenderableInde = 0xFFFFFFFFu;
    // Note that the renderable index is at most 24 bits
    virtual uint32_t GetInstanceCustomIndex () const { return kInvalidRenderableInde; }

    virtual bool IsEmpty () const ;

    FORCEINLINE bool IsTransformDirty () const { return transform_dirty_; }
    FORCEINLINE void SetTransformDirty (bool dirty) { transform_dirty_ = dirty; }
    FORCEINLINE RenderableType GetType() const { return type_; }
    virtual void Update (RendererView * view, RenderGraphBuilder& builder) = 0;

    FORCEINLINE void SetTransform (const Transform& transform) {
        transform_dirty_ = true;
        transform_ = transform;
    }
    FORCEINLINE const Transform& GetTransform () const {
        return transform_;
    }

    FORCEINLINE Transform & EditTransform () {
        transform_dirty_ = true;
        return transform_;
    }

    // Index of the renderable within its world
    FORCEINLINE uint32_t GetIndex () const {
        mi_assert(index_ != UINT32_MAX, "Index is not set.");
        return index_;
    }

    template<typename T>
    FORCEINLINE T* As () {
        if (type_ == T::kRenderableType) {
            return static_cast<T*>(this);
        }
        return nullptr;
    }

    virtual RenderableHeader GetDeviceRenderableHeader () const ;

    FORCEINLINE bool IsValid () const {
        return index_ != UINT32_MAX;
    }

    FORCEINLINE const AABB & GetAABB () const {
        return aabb_;
    }

protected:

    Renderable(RenderableType type, Scene * scene);
    Transform transform_;
    Scene * scene_;
    uint32_t index_ {UINT32_MAX};

    AABB aabb_ {}; // Axis-aligned bounding box of the renderable, used for culling & bounds calculation

    // Invisible renderables wont be rendered.
    bool visible_ {true};

    // Dirty means the data associated with the renderable (except transform) needs to be updated on device.
    bool dirty_ {true};

    // Transform dirty means the transform has changed.
    bool transform_dirty_ {true};

    // If the renderable is ray-traced. Ray traced renderbles must override GetBLAS() and GetInstanceCustomIndex() methods
    bool ray_traced_ {true};

    RenderableType type_ {RenderableType::kStaticMeshInstance};

};

MI_NAMESPACE_END

#endif //MI_RENDERABLE_H
