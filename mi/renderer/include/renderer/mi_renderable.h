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

enum class RenderableFlagBits : uint32_t {
    kNone = 0,
    kVisible = 1 << 0,
    kRayTraced = 1 << 1,
};

MAKE_FLAGS(Renderable);

class Renderable : public NonMovable, public NonCopyable {
public:
    friend Scene;

    FORCEINLINE uint32_t IncRef() {
        return ++ref_count_;
    }

    FORCEINLINE uint32_t DecRef() {
        ref_count_--;
        if (ref_count_ == 0) {
            QueueForDeletion();
        }
        return ref_count_;
    }


    virtual ~Renderable();
    // Invisible renderables wont be rendered & taken into consideration by lighting.
    FORCEINLINE bool IsVisible() const { return flags_ & RenderableFlagBits::kVisible; }
    FORCEINLINE void SetVisible(bool visible) {
        if (visible) {
            flags_ |= RenderableFlagBits::kVisible;
        } else {
            flags_ &= ~RenderableFlagBits::kVisible;
        }
    }
    // Dirty means that the renderer will make a call to Update before rendering.
    FORCEINLINE bool IsDirty () const { return dirty_; }
    FORCEINLINE void SetDirty (bool dirty = true) { dirty_ = dirty; }

    // If the renderable is ray-traced. Ray traced renderables must override GetBLAS() and GetInstanceCustomIndex().
    FORCEINLINE bool IsRayTraced () const { return flags_ & RenderableFlagBits::kRayTraced; }
    FORCEINLINE void SetRayTraced (bool ray_traced) {
        if (ray_traced != IsRayTraced()) {
            if (ray_traced) {
                flags_ |= RenderableFlagBits::kRayTraced;
            } else {
                flags_ &= ~RenderableFlagBits::kRayTraced;
            }
            SetDirty();
        }
    }

    // Override the functions if the renderable can be ray-traced.
    virtual RHIAccelerationStructure * GetBLAS () const { return nullptr; }
    constexpr static uint32_t kInvalidRenderableIndex = 0xFFFFFFFFu;
    // Note that the renderable index is at most 24 bits
    virtual uint32_t GetInstanceCustomIndex () const { return kInvalidRenderableIndex; }
    FORCEINLINE uint32_t GetHash() const { return hash_; }

    virtual bool IsEmpty () const ;

    FORCEINLINE bool IsTransformDirty () const { return transform_dirty_; }
    FORCEINLINE void SetTransformDirty (bool dirty = true) { transform_dirty_ = dirty; }
    FORCEINLINE bool ClearTransformDirty () {
        bool was_dirty = transform_dirty_;
        transform_dirty_ = false;
        return was_dirty;
    }
    FORCEINLINE RenderableType GetType() const { return type_; }

    // Called for dirty renderables before rendering each frame by the renderer.
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

    FORCEINLINE RenderableFlags GetRenderableFlags () const { return flags_; }

protected:


    uint32_t ref_count_ {};

    Transform transform_ {};
    Scene * scene_;
    uint32_t index_ {UINT32_MAX};
    uint32_t hash_ {0};

    RenderableFlags flags_ {RenderableFlagBits::kVisible | RenderableFlagBits::kRayTraced};

    // Axis-aligned bounding box of the renderable in object space, used for culling & bounds calculation
    // Should be updated in Update().
    AABB aabb_ {};


    // Dirty means the data associated with the renderable (except transform) needs to be updated on device.
    bool dirty_ {true};

    // Transform dirty means the transform has changed.
    bool transform_dirty_ {true};

    RenderableType type_ {RenderableType::kStaticMeshInstance};

    Renderable(RenderableType type, Scene * scene);

    void QueueForDeletion ();

};

MI_NAMESPACE_END

#endif //MI_RENDERABLE_H
