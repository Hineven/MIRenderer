/*
 * Created: 2025/11/23
 * Author: Exploring Air Joe
 * See LICENSE for licensing.
 */

#ifndef MI_VOLUME_GRID_H
#define MI_VOLUME_GRID_H

#include <span>
#include <vector>

#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_renderer_fwd.h"
#include "renderer/mi_volume_texture.h"
#include <rdg/rdg_ray_tracing_registry.h>

#include "../shaders/shared/SharedVolumeGrid.hlsl"

MI_NAMESPACE_BEGIN

// 对应 GPU 端的资源 Header 结构 (通常上传到 Allocator 管理的 Buffer 中)
struct VolumeGridHeader;

// ------------------------------------------------------------------
// DeviceVolumeGrid
// ------------------------------------------------------------------
// 负责管理 GPU 端的资源句柄 (Bindless Slot) 和加速结构 (BLAS)
class DeviceVolumeGrid : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    friend class VolumeGrid;

    FORCEINLINE bool IsValid () const {
        return slot_ && slot_->Get() != UINT32_MAX;
    }
    FORCEINLINE uint32_t GetIndex () const {
        return slot_ ? slot_->Get() : UINT32_MAX;
    }

    FORCEINLINE RHIAccelerationStructure * GetBLAS () const {
        return BLAS_.Raw();
    }

protected:
    DeviceVolumeGrid (DeviceBindlessResourceAllocator * allocator) ;
    ~DeviceVolumeGrid() ;

    // Index keeper of the volume grid slot (assigned by the allocator, delayed free).
    TRef<DeviceBindlessResourceAllocator::SlotKeeper> slot_;

    TRef<RHIAccelerationStructure> BLAS_;
};

// ------------------------------------------------------------------
// VolumeGrid (Asset)
// ------------------------------------------------------------------
// 代表一个体积资源 (类似于 Mesh 或 Material)。
// 它持有 3D 纹理数据和资源的原始物理属性 (如 VDB 的原始大小)。
class VolumeGrid : public NonMovable, public NonCopyable, public RefCounted<> {
public:
    friend class Renderer;

    static TRef<VolumeGrid> Create ();

    // 设置存储体素数据的 3D 纹理
    void SetTexture(TRef<VolumeTexture> texture);
    FORCEINLINE VolumeTexture* GetTexture() const { return texture_.Raw(); }

    FORCEINLINE DeviceVolumeGrid * GetDeviceVolumeGrid () const {
        return device_volume_grid_.Raw();
    }

    // 设置资源的原始局部包围盒 (来自 VDB Metadata)
    void SetAABB(const AABB& aabb);
    FORCEINLINE AABB GetAABB() const { return aabb_; }

    void UpdateOnDevice_Async (DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) ;
    void UpdateOnDevice (DeviceBindlessResourceAllocator * alloc) ;

    FORCEINLINE bool IsDirty() const { return dirty_; }
    void SetDirty(bool dirty = true);

    FORCEINLINE bool IsRayTraced () const {
        return ray_traced_;
    }
    FORCEINLINE void SetRayTraced (bool ray_traced) {
        if (ray_traced != ray_traced_) {
            ray_traced_ = ray_traced;
            SetDirty();
        }
    }

    FORCEINLINE bool IsDynamic () const {
        return dynamic_;
    }
    FORCEINLINE void SetDynamic (bool dynamic) {
        if (dynamic != dynamic_) {
            dynamic_ = dynamic;
            SetDirty();
        }
    }

protected:
    TRef<DeviceVolumeGrid> device_volume_grid_;
    TRef<VolumeTexture> texture_;


    AABB aabb_ {};

    bool dirty_ { true };

    bool ray_traced_ {true}; // Whether the volume grid is used for ray tracing.

    bool dynamic_ {false}; // Whether the volume grid is dynamic (can be updated frequently)

    DirtyTracker<VolumeGrid>* tracker_ {};
};

// ------------------------------------------------------------------
// VolumeGridInstance (Scene Object)
// ------------------------------------------------------------------
// 代表场景中的一个体积实例。
class VolumeGridInstance : public Renderable {
public:
    static TRef<VolumeGridInstance> Create(Scene* scene, VolumeGrid* volume_grid, Transform transform = {});

    // 返回包含 DeviceVolumeGrid 索引的 Header，用于构建 Instance Data
    RenderableHeader GetDeviceRenderableHeader() const override;

    constexpr static RenderableType kRenderableType = RenderableType::kVolumeGridInstance;

    FORCEINLINE VolumeGrid * GetVolumeGrid() const {
        return volume_grid_.Raw();
    }

    // 更新AABB包围盒
    void Update(RendererView* view, RenderGraphBuilder& builder);

    RHIAccelerationStructure* GetBLAS() const override;
    uint32_t GetInstanceCustomIndex() const override;
    uint32_t GetRayTracedClassIndex() const override;

    static RayTracedRenderableClassRegistrator<VolumeGridInstance> kClassRegistrator;

    bool IsEmpty() const override;

protected:
    VolumeGridInstance(Scene* scene);
    ~VolumeGridInstance() override;

    TRef<VolumeGrid> volume_grid_;
};

MI_NAMESPACE_END

#endif //MI_VOLUME_GRID_H
