/*
 * Created: 2025/5/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RDG_HELPER_H
#define RDG_HELPER_H

#include "rdg/rdg.h"
MI_NAMESPACE_BEGIN

// Simple helpers for easily adding commonly used RDG passes. As well as invoking raw RHI commands.
class Helpers {
public:
    // Spawn a pass that creates a dispatch indirect command with the specified number of thread groups.
    static TRef<RDGBuffer> SpawnDispatchIndirectCommand1D (RenderGraphBuilder & builder, RDGBuffer * count_buffer, uint32_t up_divisor = 1);

    // Enqueue upload commands to the RHI graphics command queue and place barriers.
    // If you want that happen immediately, launch a submit on the queue and wait idle.
    static void Upload_Async (RHIBufferSpan buffer, const void * data, size_t size) ;
    // You should manually barrier / wait for idle on the queue before the buffer is used.
    static void Upload_Async (RHICommandQueueGraphics & queue, RHIBufferSpan buffer, const void * data, size_t size) ;
    // You should manually barrier / wait for idle on the queue before the buffer is used.
    static void Upload_Async (RHICommandQueueGraphics & queue, RHITexture * texture, const void * data, size_t size, RHITextureLayoutType dst_layout, RHIGPUAccessFlags dst_access) ;
    // You should manually barrier / wait for idle on the queue before the buffer is used.
    template<CMemTrivial T>
    static void Upload_Async (RHICommandQueueGraphics & queue, RHIBuffer * buffer, size_t offset, const T & data) {
        Upload_Async(queue, RHIBufferSpan{buffer, offset, sizeof(T)}, &data, sizeof(T));
    }
    // Add a RDG pass to upload data to a buffer. Will not track buffer usage in RDG.
    static void UploadWithRDG_Unsafe (RenderGraphBuilder & builder, RHIBufferSpan buffer, const void * data) ;
    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDG (RenderGraphBuilder & builder, RDGBuffer * buffer, const void * data, size_t size, size_t dst_offset = 0) ;
    // Add a RDG pass to copy data back to host memory.
    static void ReadbackWithRDG (RenderGraphBuilder & builder, RDGBuffer * buffer, size_t src_offset, RHIBufferSpan readback_buffer) ;
    // Add a RDG pass to copy data back to host memory. Will not track buffer usage in RDG.
    static void ReadbackWithRDG_Unsafe (RenderGraphBuilder & builder, RHIBufferSpan buffer, RHIBufferSpan readback_buffer) ;
    // Copy back data to host memory.
    static void Readback (RHICommandQueueGraphics & queue, RHIBufferSpan buffer, void * data) ;

    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDGUsingStagingBuffer (RenderGraphBuilder & builder, RHIBufferSpan buffer, RHIBufferSpan staging_buffer, const void * data, size_t size) ;
};

MI_NAMESPACE_END

#endif //RDG_HELPER_H
