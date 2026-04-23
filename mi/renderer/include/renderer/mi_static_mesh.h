/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_STATIC_MESH_H
#define MI_STATIC_MESH_H

#include <span>
#include <vector>

#include "mi_buffer_heap.h"
#include "mi_resource_allocator.h"
#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_fwd.h"
#include "renderer/r_light_cluster_hiearchy.h"
#include <rdg/rdg_ray_tracing_registry.h>
MI_NAMESPACE_BEGIN

// Device side of a static mesh. Holds BLAS & geometry buffers & placeholder
class DeviceStaticMesh : public NonCopyable, public NonMovable, public RefCounted<true> {
public:
    friend class StaticMesh;
    FORCEINLINE RHIAccelerationStructure * GetBLAS () const {
        return BLAS_.Raw();
    }
    FORCEINLINE bool IsValid () const {
        return slot_ && (slot_->Get() != UINT32_MAX);
    }
    FORCEINLINE uint32_t GetIndex () const {
        return slot_ ? slot_->Get() : UINT32_MAX;
    }
protected:
    DeviceStaticMesh (DeviceBindlessResourceAllocator * allocator) ;
    ~DeviceStaticMesh() ;
    // Store a list of material indices on the device
    TRef<DeviceUberBufferAllocation> geometry_material_indices_;
    // If this static mesh is ray-traced, it should have a bottom-level acceleration structure.
    TRef<RHIAccelerationStructure> BLAS_;

    // Index keeper of the static mesh slot (assigned by the allocator, delayed free).
    TRef<DeviceBindlessResourceAllocator::SlotKeeper> slot_;

    DeviceBindlessResourceAllocator * allocator_ {};
};

// Static mesh, holds a "static mesh" assembled from pairs of geometry and material.
class StaticMesh : public NonCopyable, public NonMovable, public RefCounted<true> {
public:
    struct MeshLightHierarchyRecord {
        uint32_t static_mesh_local_geometry_index {};
        MeshLightClusterHierarchy hierarchy {};
        // Number of clusters for each tree depth level. Level 0 is the root cluster level.
        std::vector<uint32_t> depth_level_cluster_counts {};
    };

    friend class Material;
    friend class StaticMeshInstance;

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
        if (dirty) {
            light_hierarchy_dirty_ = true;
            light_hierarchy_device_dirty_ = true;
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

    // Build mesh-light hierarchies on CPU and cache them on this static mesh.
    // The cache is shared across all instances referencing this static mesh.
    void RebuildLightClusterHierarchy_CPU(const MeshLightClusterBuildConfig & config = {});

    // Upload persistent cluster headers/nodes to allocator uber buffers.
    // This is independent from legacy RawLight generation.
    void UploadLightClusterHierarchy_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue);

    FORCEINLINE const std::vector<MeshLightHierarchyRecord> & GetLightHierarchyRecords() const {
        return light_hierarchy_records_;
    }

    FORCEINLINE const std::vector<MeshLightInstance> & GetMeshLightInstanceTemplate() const {
        return mesh_light_instance_template_;
    }

    FORCEINLINE const std::vector<MeshLight> & GetMeshLights() const {
        return mesh_lights_;
    }

    FORCEINLINE const std::vector<MeshLightLevelHeader> & GetMeshLightLevelHeaders() const {
        return mesh_light_level_headers_;
    }

    FORCEINLINE DeviceStaticMesh * GetDeviceStaticMesh () const {
        return device_static_mesh_.Raw();
    }

    static TRef<StaticMesh> Create (bool is_ray_traced = true, bool dynamic = false);

    FORCEINLINE const AABB & GetAABB () const {
        return aabb_;
    }

    FORCEINLINE const StaticMeshHeader & GetHeader() const {
        return header_;
    }

    // Visibility buffer reserved 8 bits for static mesh descriptor index. So the max number is 256.
    constexpr static uint32_t kMaxNumGeometries = 256;

    FORCEINLINE bool HasDoubleSidedMaterial () const {
        // Cached variable to check if we can use face-culling on this mesh.
        return has_double_sided_material_;
    }

protected:
    std::vector<TRef<Geometry>> geometries_;
    std::vector<TRef<Material>> materials_;

    StaticMeshHeader header_ {};

    AABB aabb_ {};

    TRef<DeviceStaticMesh> device_static_mesh_;

    // If this static mesh is ray-traced. If true, it should have an acceleration structure.
    bool is_ray_traced_ {};

    // If true, the mesh is being prepared to work faster with frequent geometry updates.
    bool dynamic_ {};

    // Dirty bit. If dirty, the object is updated on the host but not on the device.
    bool dirty_ {true};

    // Marked when geometry/material changed and CPU hierarchy cache must rebuild.
    bool light_hierarchy_dirty_ {true};

    // Marked when hierarchy upload to GPU uber buffers is stale.
    bool light_hierarchy_device_dirty_ {true};

    // Cached variable to check if we can use face-culling on this mesh.
    bool has_double_sided_material_ {false};

    std::vector<MeshLightHierarchyRecord> light_hierarchy_records_;

    // MeshLights
    std::vector<MeshLightTriangle> mesh_light_triangles_;
    std::vector<MeshLightTriangleHash> mesh_light_triangle_hashes_;
    std::vector<MeshLightTriangleBakedData> mesh_light_triangle_baked_data_;
    std::vector<MeshLightLevelHeader> mesh_light_level_headers_;
    std::vector<MeshLight> mesh_lights_;
    // MeshLightInstance template generated from hierarchy records.
    // RenderableIndex is filled per-instance at upload time.
    std::vector<MeshLightInstance> mesh_light_instance_template_;

    // Persistent shared allocations owned by static mesh and reused by all instances.
    TRef<DeviceUberBufferArrayAllocation> light_triangle_streams_;
    TRef<DeviceUberBufferArrayAllocation> light_cluster_streams_;
    TRef<DeviceUberBufferAllocation> light_level_headers_;
    TRef<DeviceUberBufferAllocation> lights_;

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

    RHIAccelerationStructure * GetBLAS() const override ;
    RHIASGeometryInstanceFlags GetASGeometryInstanceFlags() const override ;
    uint32_t GetInstanceCustomIndex() const override;
    uint32_t GetRayTracedClassIndex() const override;

    static RayTracedRenderableClassRegistrator<StaticMeshInstance> kClassRegistrator;

    bool IsEmpty() const override;

    FORCEINLINE const std::vector<MeshLightInstance> & GetMeshLightInstances() const {
        return mesh_light_instances_;
    }

    FORCEINLINE DeviceUberBufferAllocation * GetMeshLightInstanceBufferAllocation() const {
        return mli_buffer_.Raw();
    }

protected:
    StaticMeshInstance(Scene * world) ;
    ~StaticMeshInstance() override;

    // A buffer storing the lights for this static mesh, used for lighting calculations.
    // Leave empty for static meshes with no emissive materials.
    // TRef<DeviceUberBufferAllocation> lights_;

    // Per-instance mesh-light instance buffers templated from the underlying static mesh.
    std::vector<MeshLightInstance> mesh_light_instances_;
    TRef<DeviceUberBufferAllocation> mli_buffer_;
    // Buffer allocations to hold the instance buffers alive.
    std::vector<TRef<DeviceUberBufferArrayAllocation>> mli_cluster_buffers_;
    std::vector<TRef<DeviceUberBufferAllocation>> mli_triangle_buffers_;

    TRef<StaticMesh> static_mesh_ {}; // The static mesh this instance is linked to
};


MI_NAMESPACE_END

#endif //MI_STATIC_MESH_H
