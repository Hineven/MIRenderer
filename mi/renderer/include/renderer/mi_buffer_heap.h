/*
 * Created: 2025/5/2
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_BUFFER_HEAP_H
#define MI_BUFFER_HEAP_H

// Allocate buffer segments on a single buffer or a few buffers.
// Reduce fragmentation and number of bindings when invocating shaders. (Bind entire heaps with a few bindings)

#include <set>

#include "core/common.h"
#include "core/infra.h"
#include "rhi/rhi_desc.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_types.h"

MI_NAMESPACE_BEGIN

// Interface for resource level buffer allocator
class DeviceBufferHeapInterface : public NonMovable, public NonCopyable, public RefCounted<> {
public:
    friend class DeviceBufferHeapBuffer;
    DeviceBufferHeapInterface (RHIBufferUsageFlags usage, uint32_t alignment) : usage_(usage), allocation_alignment(alignment) {}
    virtual RHIBufferSpan Allocate (uint32_t size) = 0;
    // Allocate a reference counted buffer.
    TRef<DeviceBufferHeapBuffer> AllocateRefCounted (uint32_t size) ;
    virtual void Free (RHIBufferSpan allocation) = 0;
    FORCEINLINE uint32_t GetAllocationAlignment () const {
        return allocation_alignment;
    }
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

// A very simple buffer heap for grouping up one kind of memory in buffer resource level
class SimpleDeviceBufferHeap : public DeviceBufferHeapInterface {
protected:
    // Default size of a buffer block in bytes
    uint32_t default_buffer_block_size_ {};
    struct BufferBlock {
        BufferBlock();
        ~BufferBlock();
        TRef<RHIBuffer> buffer;
        struct Segment {
            size_t start_offset {};
            // This does not affect orders so mutable.
            mutable size_t size {};
            FORCEINLINE bool operator < (const Segment & rhs) const {
                return start_offset < rhs.start_offset;
            }
        };
        std::set<Segment> free_segments_;
    };
    std::vector<BufferBlock> buffer_blocks_;
    int FindBufferBlockIndex (RHIBuffer * buffer) const ;

    void AddNewBlock (size_t block_size, size_t first_allocation_size);

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

    void SetName (const std::string & name) override;

    void PreAllocateBlocks(uint32_t num_blocks) override;

protected:

    // 0 for unlimited
    uint32_t max_num_buffer_blocks_ {};
    std::mutex mutex_;
};

// TODO write a better implementation for buffer heaps.
using DefaultDeviceBufferHeap = SimpleDeviceBufferHeap;

MI_NAMESPACE_END

#endif //MI_BUFFER_HEAP_H
