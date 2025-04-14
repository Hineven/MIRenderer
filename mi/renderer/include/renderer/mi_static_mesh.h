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

#include "core/base.h"
#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN
class RHIBuffer;

// Default vertex format
struct DefaultStaticMeshVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

class Geometry : public RefCounted<>, public NonMovable {
protected:
    Geometry();
    ~Geometry() override;

    std::vector<DefaultStaticMeshVertex> vertices_;
    std::vector<uint32_t> indices_;

    // Device related data
    TRef<RHIBuffer> device_vertex_buffer_;
    size_t device_vertex_buffer_offset_;
    TRef<RHIBuffer> device_index_buffer_;
    size_t device_index_buffer_offset_;
    // uint32_t device_vertex_count_;
    // uint32_t device_index_count_;
public:
    friend class StaticMesh;
    bool dirty_ {false};
    static TRef<Geometry> CreateFromVertices (
        std::span<DefaultStaticMeshVertex> vertices,
        std::span<uint32_t> indices
    ) ;
    static TRef<Geometry> CreateFromVertices (
        std::span<DefaultStaticMeshVertex> vertices
    ) ;

    FORCEINLINE uint32_t GetVertexCount () const {return vertices_.size();}
    FORCEINLINE uint32_t GetIndexCount () const {return indices_.size();}

    FORCEINLINE RHIBuffer * GetDeviceVertexBuffer () const {
        return device_vertex_buffer_.Raw();
    }
    FORCEINLINE size_t GetDeviceVertexBufferOffset () const {
        return device_vertex_buffer_offset_;
    }
    FORCEINLINE RHIBuffer * GetDeviceIndexBuffer () const {
        return device_index_buffer_.Raw();
    }
    FORCEINLINE size_t GetDeviceIndexBufferOffset () const {
        return device_index_buffer_offset_;
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
