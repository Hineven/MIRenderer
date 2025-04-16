/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_STATIC_MESH_H
#define MI_STATIC_MESH_H

#include <memory>
#include <span>
#include <vector>

#include "mi_world.h"
#include "core/base.h"
#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "rhi/rhi_fwd.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

class DeviceGeometry : public RefCounted<>, public NonMovable {
protected:
    DeviceGeometry(RenderResourceAllocator * allocator);
    ~DeviceGeometry() override;

    RenderResourceAllocator * allocator_ {};

    // Device related data
    TRef<GPUBufferHeapBuffer> vertex_buffer_;
    TRef<GPUBufferHeapBuffer> index_buffer_;
    // The first index to draw of the geometry in the device index buffer.
    uint32_t first_index_ {};
    uint32_t vertex_count_ {};
    uint32_t index_count_ {};
    bool dirty_ {false};
public:

    friend class Geometry;

    FORCEINLINE uint32_t GetVertexCount () const {return vertex_count_;}
    FORCEINLINE uint32_t GetIndexCount () const {return index_count_;}

    FORCEINLINE RHIBufferSpan GetDeviceVertexBuffer () const {
        return vertex_buffer_->GetRHI();
    }
    FORCEINLINE RHIBufferSpan GetDeviceIndexBuffer () const {
        return index_buffer_->GetRHI();
    }

    FORCEINLINE size_t GetDeviceFirstIndex () const {
        return first_index_;
    }
};

class Geometry : public RefCounted<>, public NonMovable {
protected:
    Geometry();
    ~Geometry() override;

    std::vector<DefaultStaticMeshVertex> vertices_;
    std::vector<uint32_t> indices_;

    TRef<DeviceGeometry> device_geometry_;

    bool dirty_ {false};
public:
    friend class StaticMesh;
    static TRef<Geometry> CreateFromVertices (
        std::span<DefaultStaticMeshVertex> vertices,
        std::span<uint32_t> indices = {}
    ) ;

    FORCEINLINE uint32_t GetVertexCount () const {return vertices_.size();}
    FORCEINLINE uint32_t GetIndexCount () const {return indices_.size();}

    FORCEINLINE DeviceGeometry * GetDeviceGeometry () const {
        return device_geometry_.Raw();
    }

    void CreateOnDevice (RenderResourceAllocator * alloc);
    void UpdateOnDevice ();
    void ReleaseHost ();
    void ReleaseDevice ();

    FORCEINLINE bool IsHostPresent () {
        return vertices_.data() != nullptr && indices_.data() != nullptr;
    }
    FORCEINLINE bool IsDevicePresent () {
        return device_geometry_;
    }
};

class StaticMesh : public Renderable {
public:
    StaticMesh() ;
    ~StaticMesh() override;
    void AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) ;
    void Update(RenderGraphBuilder &builder) override;

    FORCEINLINE const std::vector<TRef<Geometry>> & GetGeometries () const { return geometries_; }
    FORCEINLINE const std::vector<TRef<Material>> & GetMaterials () const { return materials_; }
protected:
    std::vector<TRef<Geometry>> geometries_;
    std::vector<TRef<Material>> materials_;
};


MI_NAMESPACE_END

#endif //MI_STATIC_MESH_H
