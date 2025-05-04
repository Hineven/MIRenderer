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

class WorldDeviceData : public NonCopyable, public NonMovable {
public:
    friend class World;

    // Indexed with renderable index.
    TRef<RHIBuffer> renderable_transforms_;
    TRef<RHIBuffer> renderable_headers_;

    // Record the index of the material of each geometry from all static mesh renderables.
    // This buffer heap is limited to 1 buffer block.
    TRef<DeviceBufferHeapInterface> static_mesh_renderable_materials_;

    // TRef<RHIBindlessSlotKeeper<RHITexture>> sky_texture_;

protected:
    WorldDeviceData();
    ~WorldDeviceData();
};

// Integrated class managing the world.
// It is responsible for holding renderables and rendering resources of a "3d world“。
class World : public NonCopyable, public NonMovable {
public:

    friend class Renderable;

    constexpr static uint32_t kMaxNumRenderables = 4096;
    constexpr static uint32_t kMaxNumStaticMeshGeometryMaterialPairs = 4096 * 16;

    void RemoveRenderable (Renderable * renderable) ;

    FORCEINLINE const std::vector<TRef<Renderable>> & GetRenderables () const {
        return renderables_;
    }

    FORCEINLINE WorldDeviceData * GetDevice () const {
        return device_world_.get();
    }

    void SetSkyTexture (Texture * texture) ;
    FORCEINLINE TRef<Texture> GetSkyTexture () const {
        return sky_texture_;
    }

    FORCEINLINE bool IsDevicePresent () const {
        return device_world_ != nullptr;
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

    TRef<Texture> sky_texture_;

    std::unique_ptr<WorldDeviceData> device_world_;
};

MI_NAMESPACE_END

#endif //MI_WORLD_H
