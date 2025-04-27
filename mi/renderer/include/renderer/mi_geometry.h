/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_GEOMETRY_H
#define MI_GEOMETRY_H

#include <vector>

#include "core/base.h"
#include "core/refcounted.h"
#include "rhi/rhi_desc.h"
#include "renderer/mi_renderer_fwd.h"
MI_NAMESPACE_BEGIN

class DeviceGeometry : public RefCounted<>, public NonMovable {
protected:
    DeviceGeometry(RenderResourceAllocator * allocator);
    ~DeviceGeometry() override;

    RenderResourceAllocator * allocator_ {};

    // Device related data (manually released to the allocator)
    RHIBufferSpan vertex_buffer_;
    RHIBufferSpan index_buffer_;
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
        return vertex_buffer_;
    }
    FORCEINLINE RHIBufferSpan GetDeviceIndexBuffer () const {
        return index_buffer_;
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

    // If the geometry is modified on host and requires a rebuild on device.
    bool dirty_ {false};
public:
    friend class StaticMesh;
    static TRef<Geometry> CreateFromVertices (
        std::span<DefaultStaticMeshVertex> vertices = {},
        std::span<uint32_t> indices = {}
    ) ;
    FORCEINLINE static TRef<Geometry> Create () {return CreateFromVertices();}

    FORCEINLINE bool Empty () const {return vertices_.empty();}
    
    FORCEINLINE uint32_t GetVertexCount () const {return (uint32_t)vertices_.size();}
    FORCEINLINE uint32_t GetIndexCount () const {return (uint32_t)indices_.size();}

    FORCEINLINE size_t GetVertexBufferSize () const {
        return vertices_.size() * sizeof(DefaultStaticMeshVertex);
    }
    FORCEINLINE size_t GetIndexBufferSize () const {
        return indices_.size() * sizeof(uint32_t);
    }

    FORCEINLINE DeviceGeometry * GetDeviceGeometry () const {
        return device_geometry_.Raw();
    }

    FORCEINLINE bool IsDirty () const {
        return dirty_;
    }

    void SetName (std::string_view name);

    void CreateOnDevice (RenderResourceAllocator * alloc);
    // Update on device. This function synchronizes with RHI thread directly.
    void SyncAndUpdateOnDevice ();
    void ReleaseHost ();
    void ReleaseDevice ();

    FORCEINLINE bool IsHostPresent () {
        return !vertices_.empty() && !indices_.empty();
    }
    FORCEINLINE bool IsDevicePresent () {
        return device_geometry_;
    }
};

MI_NAMESPACE_END
#endif //MI_GEOMETRY_H
