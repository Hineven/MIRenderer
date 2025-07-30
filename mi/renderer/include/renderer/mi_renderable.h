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
    // TODO add dirty renderables to a list every frame for better performance!
    FORCEINLINE void SetDirty (bool dirty) { dirty_ = dirty; }
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
    bool dirty_ {true};
    bool transform_dirty_ {true};
    RenderableType type_ {RenderableType::kStaticMeshInstance};

};

MI_NAMESPACE_END

#endif //MI_RENDERABLE_H
