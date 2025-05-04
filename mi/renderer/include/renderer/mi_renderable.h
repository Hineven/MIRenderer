/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RENDERABLE_H
#define MI_RENDERABLE_H

#include "shaders/SharedRenderable.hlsl"

#include "mi_world.h"
#include "core/base.h"
#include "core/common.h"
#include "core/infra.h"
#include "core/refcounted.h"
#include "renderer/mi_renderer_fwd.h"
#include "renderer/mi_renderer_types.h"
#include "renderer/mi_transform.h"

MI_NAMESPACE_BEGIN

class StaticMesh;

class RenderGraphBuilder;
class Renderable : public NonMovable, public RefCounted<> {
public:
    friend class World;
    virtual ~Renderable();
    FORCEINLINE bool IsVisible() const { return visible_; }
    FORCEINLINE void SetVisible(bool visible) { visible_ = visible; }
    // Dirty means that the renderer will make a call to Update before rendering.
    FORCEINLINE bool IsDirty () const { return dirty_; }
    FORCEINLINE void SetDirty (bool dirty) { dirty_ = dirty; }
    FORCEINLINE RenderableType GetType() const { return type_; }
    virtual void Update (RendererView * view, RenderGraphBuilder& builder) = 0;

    FORCEINLINE void SetTransform (const Transform& transform) {
        transform_ = transform;
        dirty_ = true;
    }
    FORCEINLINE const Transform& GetTransform () const {
        return transform_;
    }

    // Index of the renderable within its world
    FORCEINLINE uint32_t GetIndex () const {
        mi_assert(index_ != UINT32_MAX, "Index is not set.");
        return index_;
    }

    template<typename T>
    FORCEINLINE T* As () {return static_cast<T*>(this);}

    virtual RenderableHeader GetDeviceRenderableHeader () const ;

protected:

    // Proxy for World::AllocateRenderableIndex();
    static uint32_t AllocateRenderableIndexFromWorld (World * world) ;

    Renderable(RenderableType type, uint32_t index, World * world);

    Transform transform_;
    World * world_;
    uint32_t index_ {UINT32_MAX};

    // Invisible renderables wont be rendered.
    bool visible_ {true};
    bool dirty_ {false};
    RenderableType type_ {RenderableType::kStaticMesh};

};

template<CMemTrivial TDst, CMemTrivial TSrc>
const TDst & ReinterpretAs (const TSrc & src) {
    static_assert(sizeof (TSrc) == sizeof(TDst), "Size mismatch");
    static_assert(alignof (TSrc) == alignof (TDst), "Alignment mismatch");
    return *reinterpret_cast<const TDst *>(&src);
}

MI_NAMESPACE_END

#endif //MI_RENDERABLE_H
