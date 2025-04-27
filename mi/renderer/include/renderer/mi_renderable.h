/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERABLE_H
#define MI_RENDERABLE_H

#include "core/base.h"
#include "core/common.h"
#include "core/refcounted.h"
#include "renderer/mi_renderer_types.h"
#include "renderer/mi_transform.h"

MI_NAMESPACE_BEGIN

class StaticMesh;

class RenderGraphBuilder;
class Renderable : public NonMovable, public RefCounted<> {
public:
    virtual ~Renderable();
    FORCEINLINE bool IsVisible() const { return visible_; }
    FORCEINLINE void SetVisible(bool visible) { visible_ = visible; }
    // Dirty means that the renderer will make a call to Update before rendering.
    FORCEINLINE bool IsDirty () const { return dirty_; }
    FORCEINLINE void SetDirty (bool dirty) { dirty_ = dirty; }
    FORCEINLINE RenderableType GetType() const { return type_; }
    virtual void Update (RenderGraphBuilder& builder) = 0;

    FORCEINLINE void SetTransform (const Transform& transform) {
        transform_ = transform;
        dirty_ = true;
    }
    FORCEINLINE const Transform& GetTransform () const {
        return transform_;
    }

    template<typename T>
    FORCEINLINE T* As () {return static_cast<T>(this);}

protected:

    Transform transform_;

    Renderable(RenderableType type);
    // Invisible renderables wont be rendered.
    bool visible_ {true};
    bool dirty_ {false};
    RenderableType type_ {RenderableType::kStaticMesh};
};


MI_NAMESPACE_END

#endif //MI_RENDERABLE_H
