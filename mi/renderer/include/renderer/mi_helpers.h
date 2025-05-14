/*
 * Created: 2025/4/14
 * Author:  *hineven
 * See LICENSE for licensing.
 */

#ifndef MI_HELPERS_H
#define MI_HELPERS_H

#include "core/common.h"
#include <renderer/mi_renderer_fwd.h>
#include <rhi/rhi_desc.h>

MI_NAMESPACE_BEGIN
    class RHIBuffer;
class RDGBuffer;
class RenderGraphBuilder;
class Helpers {
public:
    // Enqueue upload commands to the RHI graphics command queue and place barriers.
    // If you want that happen immediately, launch a submit on the queue and wait idle.
    static void Upload_Async (RHIBufferSpan buffer, const void * data, size_t size) ;
    // You should manually barrier / wait for idle on the queue before the buffer is used.
    static void Upload_Async (RHICommandQueueGraphics & queue, RHIBufferSpan buffer, const void * data, size_t size) ;
    // You should manually barrier / wait for idle on the queue before the buffer is used.
    static void Upload_Async (RHICommandQueueGraphics & queue, RHITexture * texture, const void * data, size_t size, RHITextureLayoutType dst_layout, RHIGPUAccessFlags dst_access) ;
    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDG (RenderGraphBuilder & builder, RHIBufferSpan buffer, const void * data, size_t size) ;
    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDG (RenderGraphBuilder & builder, RDGBuffer * buffer, const void * data, size_t size, size_t dst_offset = 0) ;
    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDGUsingStagingBuffer (RenderGraphBuilder & builder, RHIBufferSpan buffer, RHIBufferSpan staging_buffer, const void * data, size_t size) ;
};

MI_NAMESPACE_END
#endif //MI_HELPERS_H
