/*
 * Created: 2025/4/16
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_GEOMETRY_H
#define MI_GEOMETRY_H

#include <vector>

#include "mi_aabb.h"
#include "mi_dirty_tracker.h"
#include "mi_renderer.h"
#include "core/base.h"
#include "core/refcounted.h"
#include "rhi/rhi_desc.h"
#include "renderer/mi_renderer_fwd.h"
#include "../../shaders/shared/SharedVertex.hlsl"
MI_NAMESPACE_BEGIN

// Device side of a geometry. Owned by Geometry and allocated by DeviceBindlessResourceAllocator.
class DeviceGeometry : public RefCounted<>, public NonMovable {
protected:
    DeviceGeometry(DeviceBindlessResourceAllocator * allocator);
    ~DeviceGeometry() override;

    DeviceBindlessResourceAllocator * allocator_ {};

    // Device related data (manually released to the allocator)
    TRef<DeviceUberBufferAllocation> vertex_buffer_;
    TRef<DeviceUberBufferAllocation> index_buffer_;
    // The first index to draw of the geometry in the bindless device index buffer.
    uint32_t first_index_ {};
    uint32_t vertex_count_ {};
    uint32_t index_count_ {};

    uint32_t index_ {UINT32_MAX}; // Index of the geometry in the bindless device allocator

    friend StaticMeshInstance;

public:

    friend class Geometry;

    FORCEINLINE uint32_t GetIndex () const {return index_;}
    FORCEINLINE bool IsValid () const {
        return index_ != UINT32_MAX;
    }

    FORCEINLINE uint32_t GetVertexCount () const {return vertex_count_;}
    FORCEINLINE uint32_t GetIndexCount () const {return index_count_;}

    FORCEINLINE DeviceUberBufferAllocation * GetDeviceVertexBuffer () const {
        return vertex_buffer_.Raw();
    }
    FORCEINLINE DeviceUberBufferAllocation * GetDeviceIndexBuffer () const {
        return index_buffer_.Raw();
    }

    FORCEINLINE uint32_t GetDeviceFirstIndex () const {
        return first_index_;
    }
};

// Geometry. Can be owned by a renderer.
class Geometry : public RefCounted<>, public NonMovable {
protected:
    Geometry();
    ~Geometry() override;

    std::vector<DefaultStaticMeshVertex> vertices_;
    std::vector<uint32_t> indices_;

    AABB aabb_ {};

    TRef<DeviceGeometry> device_geometry_;

    // If the geometry is modified on host and requires a rebuild on device.
    bool dirty_ {true};
    // Associated dirty tracker. If set, dirty tracker will track the dirty state of this geometry.
    DirtyTracker<Geometry> * tracker_ {};

public:
    friend class StaticMeshInstance;
    static TRef<Geometry> CreateFromVertices (
        std::span<DefaultStaticMeshVertex> vertices = {},
        std::span<uint32_t> indices = {}
    ) ;
    FORCEINLINE static TRef<Geometry> Create () {return CreateFromVertices();}

    FORCEINLINE void Associate (DirtyTracker<Geometry> * tracker) {
        tracker_ = tracker;
    }

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

    FORCEINLINE void SetDirty (bool dirty) {
        if (tracker_ && !dirty_ && dirty)
            tracker_->OnObjectTurnedDirty(this);
        dirty_ = dirty;
    }

    FORCEINLINE void GetIndexBuffer (std::vector<uint32_t> & indices) const {
        indices = indices_;
    }

    FORCEINLINE const AABB & GetAABB () const {
        return aabb_;
    }

    void SetName (std::string_view name);

    // Update on device. Update commands are written to the given graphics comand queue.
    // Manually submission and synchronization required for graphics queue.
    void UpdateOnDevice_Async (DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue);

    // Easy to use version. Synchronized and waits for completion.
    void UpdateOnDevice (DeviceBindlessResourceAllocator * alloc);

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
