/*
 * Created: 2025/5/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_BUFFER_HEAP_H
#define MI_BUFFER_HEAP_H

// Allocate buffer segments on a single buffer or a few buffers.
// Reduce fragmentation and number of bindings when invocating shaders. (Bind entire heaps with a few bindings)

#include <core/common.h>
#include <core/util/segment_allocator.h>
#include <rhi/rhi_desc.h>
#include <rhi/rhi_types.h>

#include <renderer/mi_delayed_destruction.h>

MI_NAMESPACE_BEGIN

// Interface for resource level buffer allocator. The buffer heap is a collection of buffer blocks
// that can allocate buffer spans and free them.
class DeviceBufferHeapInterface : public NonMovable, public NonCopyable, public RefCounted<> {
public:
    friend class DeviceBufferHeapBuffer;
    DeviceBufferHeapInterface (RHIBufferUsageFlags usage, uint32_t alignment) : usage_(usage), allocation_alignment_(alignment) {}
    virtual RHIBufferSpan Allocate (uint32_t size) = 0;
    // Allocate a reference counted buffer.
    TRef<DeviceBufferHeapBuffer> AllocateRefCounted (uint32_t size) ;
    virtual void Free (RHIBufferSpan allocation) = 0;
    FORCEINLINE uint32_t GetAllocationAlignment () const {
        return allocation_alignment_;
    }
    // Return the index of the buffer block that corresponds to the given buffer.
    virtual uint32_t GetBufferBlockIndex (RHIBuffer * buffer) const = 0;
    // Return a block buffer allocated for the buffer heap
    virtual RHIBuffer * GetHeapBufferBlock (uint32_t block_index) const = 0;
    virtual uint32_t GetNumHeapBufferBlocks () const = 0;
    virtual void SetNumBufferBlockLimit (uint32_t num) = 0;
    virtual ~DeviceBufferHeapInterface () = default;

    virtual void SetName (const std::string & name) ;
    FORCEINLINE const std::string & GetName () const {
        return name_;
    }

    // Allocate blocks even if they are unused.
    virtual void PreAllocateBlocks (uint32_t num_blocks) = 0;
protected:
    std::string name_ {};
    RHIBufferUsageFlags usage_;
    uint32_t allocation_alignment_ {};
};


class DeviceUberBufferInterface;

class DeviceUberBufferAllocation : public DelayedDestructionResource {
public:
    friend class DeviceUberBufferInterface;
    friend class DeviceBindlessResourceAllocator;

    // Offset in bytes
    [[nodiscard]] FORCEINLINE size_t GetOffset () const {
        return offset_;
    }
    // Size in bytes
    [[nodiscard]] FORCEINLINE size_t GetSize () const {
        return size_;
    }
    [[nodiscard]] FORCEINLINE DeviceUberBufferInterface * GetUberBuffer () const {
        return uber_buffer_;
    }
    [[nodiscard]] RHIBufferSpan GetRHI () const ;

protected:
    ~DeviceUberBufferAllocation() override;
    void QueueForDestruction() const override;

    // Offset and size of the allocation in the uber buffer.
    size_t offset_ {}, size_ {};
    // The uber buffer interface that this allocation belongs to.
    DeviceUberBufferInterface * uber_buffer_ {};

    // Optional owning allocator used to perform delayed destruction.
    // If null, destruction happens immediately.
    DeviceBindlessResourceAllocator * allocator_ {};
};


// A buffer heap assembled with only one buffer. Used for geometry buffers. May trigger expansion
// when allocating a new buffer segment.
class DeviceUberBufferInterface : public NonMovable, public NonCopyable, public RefCounted<> {
public:
    friend class DeviceUberBufferAllocation;
    friend class DeviceBindlessResourceAllocator;
    DeviceUberBufferInterface (RHIBufferUsageFlags usage, uint32_t alignment, DeviceBindlessResourceAllocator * allocator):
        usage_(usage), allocation_alignment(alignment), allocator_(allocator){}
    // Allocate a buffer segment from the uber buffer.
    // Be aware that the allocation may trigger an expansion of the uber buffer.
    virtual std::pair<size_t, bool> Allocate (uint32_t size, bool allow_expansion = true) = 0;
    FORCEINLINE std::pair<TRef<DeviceUberBufferAllocation>, bool> AllocateRefCounted (uint32_t size, bool allow_expansion = true) {
        auto result = Allocate(size, allow_expansion);
        if (result.first != SIZE_MAX) return {CreateAllocation(result.first, size), true};
        return {};
    }
    virtual void Free (size_t offset, size_t size) = 0;
    FORCEINLINE void Free (DeviceUberBufferAllocation * allocation) {
        Free(allocation->GetOffset(), allocation->GetSize());
    }
    FORCEINLINE uint32_t GetAllocationAlignment () const {
        return allocation_alignment;
    }
    // Underlying RHI buffer.
    virtual RHIBuffer * GetRHI () const = 0;
    virtual ~DeviceUberBufferInterface () = default;

    virtual void SetName (const std::string & name) ;
    FORCEINLINE const std::string & GetName () const {
        return name_;
    }
    // Get the byte offset of the last byte allocated + 1
    virtual size_t GetAllocationLimitByteOffset () const = 0;

protected:

    // Implementations of the interface can use this to create an allocation.
    TRef<DeviceUberBufferAllocation> CreateAllocation (size_t offset, size_t size);

    // Optional owning allocator to enable delayed destruction for sub-allocations.
    // If null, allocations will free immediately on destruction.
    DeviceBindlessResourceAllocator * allocator_ {};

    std::string name_ {};
    RHIBufferUsageFlags usage_;
    uint32_t allocation_alignment {};
};

class DeviceBufferHeapBuffer : public NonMovable, public NonCopyable, public RefCounted<> {
protected:
    FORCEINLINE DeviceBufferHeapBuffer () = default;
    RHIBufferSpan buffer {};
    TRef<DeviceBufferHeapInterface> heap {};
    friend class DeviceBufferHeapInterface;
public:
    ~DeviceBufferHeapBuffer();
    FORCEINLINE DeviceBufferHeapInterface * GetHeap () const {return heap.Raw(); }
    FORCEINLINE RHIBufferSpan GetRHI () const {return buffer; }
};

// A very simple buffer heap for grouping up one kind of memory in buffer resource level.
class SimpleDeviceBufferHeap : public DeviceBufferHeapInterface {
protected:
    // Default size of a buffer block in bytes
    uint32_t default_buffer_block_size_ {};
    struct BufferBlock {
        BufferBlock(size_t default_buffer_block_size_, size_t allocation_alignment);
        ~BufferBlock();
        TRef<RHIBuffer> buffer;
        SegmentAllocator segments_;
    };
    std::vector<BufferBlock> buffer_blocks_;
    int FindBufferBlockIndex (RHIBuffer * buffer) const ;

    void AddNewBlock (size_t block_size);

    SimpleDeviceBufferHeap (RHIBufferUsageFlags usage, uint32_t allocation_alignment, uint32_t buffer_block_size = 256 * 1024 * 1024);

public:
    ~SimpleDeviceBufferHeap();

    void SetNumBufferBlockLimit (uint32_t num) override ;

    RHIBuffer * GetHeapBufferBlock (uint32_t block_index) const override;
    uint32_t GetNumHeapBufferBlocks () const override ;

    FORCEINLINE uint32_t GetNumBufferBlockLimit () const {return max_num_buffer_blocks_;}

    RHIBufferSpan Allocate (uint32_t size) override;
    void Free (RHIBufferSpan allocation) override;

    FORCEINLINE static TRef<SimpleDeviceBufferHeap> Create (RHIBufferUsageFlags usage, uint32_t allocation_alignment, uint32_t buffer_block_size = 256 * 1024 * 1024) {
        return {new SimpleDeviceBufferHeap(usage, allocation_alignment, buffer_block_size)};
    }

    uint32_t GetBufferBlockIndex(RHIBuffer *buffer) const override;

    void SetName (const std::string & name) override;

    void PreAllocateBlocks(uint32_t num_blocks) override;

protected:

    // 0 for unlimited
    uint32_t max_num_buffer_blocks_ {};
    // TODO remove this mutex. SimpleDeviceBufferHeap should only be accessed from the render thread.
    std::mutex mutex_;
};

// A very simple uber buffer implementation.
class SimpleDeviceUberBuffer : public DeviceUberBufferInterface {
public:
    SimpleDeviceUberBuffer (RHIBufferUsageFlags usage, uint32_t allocation_alignment, size_t initial_size = 256 * 1024 * 1024,
        DeviceBindlessResourceAllocator * allocator = nullptr);
    ~SimpleDeviceUberBuffer() override;

    std::pair<size_t, bool> Allocate (uint32_t size, bool allow_expansion = true) override;
    void Free (size_t offset, size_t size) override;

    RHIBuffer * GetRHI () const override;

    FORCEINLINE static TRef<SimpleDeviceUberBuffer> Create (
        RHIBufferUsageFlags usage, uint32_t allocation_alignment, size_t initial_size = 256 * 1024 * 1024,
        DeviceBindlessResourceAllocator * allocator = nullptr
    ) {
        return {new SimpleDeviceUberBuffer(usage, allocation_alignment, initial_size, allocator)};
    }

    size_t GetAllocationLimitByteOffset() const override;

    void SetName(const std::string &name) override;

protected:
    TRef<RHIBuffer> uber_buffer_;
    SegmentAllocator segments_;
};

// TODO write a better implementation for buffer heaps.
using DefaultDeviceBufferHeap = SimpleDeviceBufferHeap;
using DefaultDeviceUberBuffer = SimpleDeviceUberBuffer;


MI_NAMESPACE_END

#endif //MI_BUFFER_HEAP_H
