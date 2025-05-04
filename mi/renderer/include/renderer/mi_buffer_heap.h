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
class DeviceBufferHeapInterface : public NonMovable, public RefCounted<> {
public:
    friend class DeviceBufferHeapBuffer;
    DeviceBufferHeapInterface (RHIBufferUsageFlags usage, uint32_t alignment) : usage_(usage), allocation_alignment(alignment) {}
    virtual RHIBufferSpan Allocate (uint32_t size) = 0;
    // Allocate a reference counted buffer.
    TRef<DeviceBufferHeapBuffer> AllocateRefCounted (uint32_t size) ;
    virtual void Free (RHIBufferSpan allocation) ;
    FORCEINLINE uint32_t GetAllocationAlignment () const {
        return allocation_alignment;
    }
    // Return a block buffer allocated for the buffer heap
    virtual RHIBuffer * GetHeapBufferBlock (uint32_t block_index) const = 0;
    virtual uint32_t GetNumHeapBufferBlocks () const = 0;
    virtual void SetNumBufferBlockLimit (uint32_t num) = 0;
    virtual ~DeviceBufferHeapInterface () = default;
protected:

    RHIBufferUsageFlags usage_;
    uint32_t allocation_alignment {};
};

class DeviceBufferHeapBuffer : public NonMovable, public RefCounted<> {
protected:
    FORCEINLINE DeviceBufferHeapBuffer () = default;
    RHIBufferSpan buffer {};
    DeviceBufferHeapInterface * heap {};
    friend class DeviceBufferHeapInterface;
public:
    ~DeviceBufferHeapBuffer();
    FORCEINLINE DeviceBufferHeapInterface * GetHeap () const {return heap; }
    FORCEINLINE RHIBufferSpan GetRHI () const {return buffer; }
};

// A very simple buffer heap for grouping up one kind of memory in buffer resource level
class SimpleDeviceBufferHeap : public DeviceBufferHeapInterface {
protected:
    uint32_t buffer_block_size_ {};
    struct BufferBlock {
        BufferBlock();
        ~BufferBlock();
        TRef<RHIBuffer> buffer;
        struct Segment {
            uint32_t start_offset {};
            // This does not affect orders so mutable.
            mutable uint32_t size {};
            FORCEINLINE bool operator < (const Segment & rhs) const {
                return start_offset < rhs.start_offset;
            }
        };
        std::set<Segment> free_segments_;
    };
    std::vector<BufferBlock> buffer_blocks_;
    int FindBufferBlockIndex (RHIBuffer * buffer) const ;
public:
    SimpleDeviceBufferHeap (RHIBufferUsageFlags usage, uint32_t allocation_alignment, uint32_t buffer_block_size = 256 * 1024 * 1024);
    ~SimpleDeviceBufferHeap();

    void SetNumBufferBlockLimit (uint32_t num) override ;

    RHIBuffer * GetHeapBufferBlock (uint32_t block_index) const override;
    uint32_t GetNumHeapBufferBlocks () const override ;

    FORCEINLINE uint32_t GetNumBufferBlockLimit () const {return max_num_buffer_blocks_;}

    RHIBufferSpan Allocate (uint32_t size) override;
    void Free (RHIBufferSpan allocation) override;

    FORCEINLINE static TRef<SimpleDeviceBufferHeap> Create (RHIBufferUsageFlags usage, uint32_t allocation_alignment, uint32_t buffer_block_size = 256 * 1024 * 1024) {
        return TRef<SimpleDeviceBufferHeap>(new SimpleDeviceBufferHeap(usage, allocation_alignment, buffer_block_size));
    }

protected:

    // 0 for unlimited
    uint32_t max_num_buffer_blocks_ {};
    std::mutex mutex_;
};

// TODO write a better implementation for buffer heaps.
using DefaultDeviceBufferHeap = SimpleDeviceBufferHeap;

MI_NAMESPACE_END

#endif //MI_BUFFER_HEAP_H
