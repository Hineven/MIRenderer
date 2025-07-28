/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_STATIC_MESH_H
#define MI_STATIC_MESH_H

#include <span>
#include <vector>

#include "mi_resource_allocator.h"
#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

// Device side of a static mesh. Holds BLAS & geometry buffers & placeholder
class DeviceStaticMesh : public NonCopyable, public NonMovable, public RefCounted<true> {
public:
    friend class StaticMesh;
    FORCEINLINE RHIAccelerationStructure * GetBLAS () const {
        return BLAS_.Raw();
    }
    FORCEINLINE bool IsValid () const {
        return index_ != UINT32_MAX;
    }
    FORCEINLINE uint32_t GetIndex () const {
        return index_;
    }
protected:
    DeviceStaticMesh (DeviceBindlessResourceAllocator * allocator) ;
    ~DeviceStaticMesh() ;
    // Store a list of material indices on the device
    TRef<DeviceUberBufferAllocation> geometry_material_indices_;
    // If this static mesh is ray-traced, it should have a bottom-level acceleration structure.
    TRef<RHIAccelerationStructure> BLAS_;

    uint32_t index_ {UINT32_MAX}; // Index of the static mesh in the bindless device allocator

    DeviceBindlessResourceAllocator * allocator_ {};
};

// Static mesh, holds a "static mesh" assembled from pairs of geometry and material.
class StaticMesh : public NonCopyable, public NonMovable, public RefCounted<true> {
public:
    FORCEINLINE const std::vector<TRef<Geometry>> & GetGeometries () const { return geometries_; }
    FORCEINLINE const std::vector<TRef<Material>> & GetMaterials () const { return materials_; }

    FORCEINLINE bool IsDynamic () const {
        return dynamic_;
    }

    FORCEINLINE void SetDynamic (bool dynamic) {
        dynamic_ = dynamic;
        SetDirty(true);
    }

    FORCEINLINE bool IsRayTraced () const {
        return is_ray_traced_;
    }

    FORCEINLINE void SetRayTraced (bool ray_traced) {
        is_ray_traced_ = ray_traced;
        SetDirty(true);
    }

    FORCEINLINE void SetDirty (bool dirty) {
        if (tracker_ && !dirty_ && dirty) {
            tracker_->OnObjectTurnedDirty(this);
        }
        dirty_ = dirty;
    }

    FORCEINLINE bool IsDirty () const {
        return dirty_;
    }

    FORCEINLINE bool IsEmpty () const {
        return geometries_.empty();
    }

    void AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) ;
    void ClearMeshPrimitives ();

    void UpdateOnDevice_Async (DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) ;

    void UpdateOnDevice (DeviceBindlessResourceAllocator * alloc);

    FORCEINLINE DeviceStaticMesh * GetDeviceStaticMesh () const {
        return device_static_mesh_.Raw();
    }

    static TRef<StaticMesh> Create (bool is_ray_traced = true, bool dynamic = false);

protected:
    std::vector<TRef<Geometry>> geometries_;
    std::vector<TRef<Material>> materials_;

    StaticMeshHeader header_ {};

    TRef<DeviceStaticMesh> device_static_mesh_;

    // If this static mesh is ray-traced. If true, it should have an acceleration structure.
    bool is_ray_traced_ {};

    // If true, the mesh is being prepared to work faster with frequent geometry updates.
    bool dynamic_ {};

    // Dirty bit. If dirty, the object is updated on the host but not on the device.
    bool dirty_ {true};

    DirtyTracker<StaticMesh> * tracker_ {};
};

// Static mesh instance, links to a static mesh. Owned by Scene.
class StaticMeshInstance : public Renderable {
public:

    void Update (RendererView * view, RenderGraphBuilder & builder);

    static TRef<StaticMeshInstance> Create (Scene * scene, StaticMesh * static_mesh, Transform transform = {}) ;

    RenderableHeader GetDeviceRenderableHeader() const override;

    constexpr static RenderableType kRenderableType = RenderableType::kStaticMeshInstance;

    FORCEINLINE StaticMesh * GetStaticMesh () const {
        return static_mesh_.Raw();
    }

    // (Re)build lights_ according to current static mesh in case of geometry & material changes.
    void UpdateLights_Async (DeviceBindlessResourceAllocator *alloc, RHICommandQueueGraphics & queue);

protected:
    StaticMeshInstance(Scene * world) ;
    ~StaticMeshInstance() override;

    // A buffer storing the lights for this static mesh, used for lighting calculations.
    // Leave empty for static meshes with no emissive materials.
    TRef<LightList> lights_;

    TRef<StaticMesh> static_mesh_ {}; // The static mesh this instance is linked to
};


MI_NAMESPACE_END

#endif //MI_STATIC_MESH_H
