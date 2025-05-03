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
#include "core/base.h"
#include "core/refcounted.h"
#include <rhi/rhi_fwd.h>

#include "mi_renderable.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

class DeviceWorld : public NonCopyable, public NonMovable {
public:
    friend class World;

    // Indexed with renderable index.
    TRef<RHIBuffer> renderable_transforms_;
    TRef<RHIBuffer> renderable_headers_;

protected:
    DeviceWorld();
    ~DeviceWorld();
};

// Integrated class managing the world.
class World : public NonCopyable, public NonMovable {
public:

    friend class Renderable;

    constexpr static uint32_t kMaxNumRenderables = 4096;
    constexpr static uint32_t kMaxNumStaticMeshGeometryMaterialPairs = 4096 * 16;

    void RemoveRenderable (Renderable * renderable) ;

    FORCEINLINE const std::vector<TRef<Renderable>> & GetRenderables () const {
        return renderables_;
    }

    FORCEINLINE DeviceWorld * GetDevice () const {
        return device_world_.get();
    }

protected:

    FORCEINLINE uint32_t AllocateRenderableIndex () {
        if (free_renderables_.empty()) {
            if (renderables_.size() < kMaxNumRenderables) {
                renderables_.emplace_back(nullptr);
                return (uint32_t)(renderables_.size() - 1);
            }
            return UINT32_MAX;
        }
        uint32_t index = free_renderables_.top();
        free_renderables_.pop();
        return index;

    }

    FORCEINLINE void FreeRenderabeIndex (uint32_t index) {
        free_renderables_.push(index);
    }

    std::vector<TRef<Renderable>> renderables_;
    // Keep track of free renderable indices, so we can reallocate them.
    std::stack<uint32_t> free_renderables_;

    std::unique_ptr<DeviceWorld> device_world_;
};

MI_NAMESPACE_END

#endif //MI_WORLD_H
