/*
 * Created: 2025/4/14
 * Author:  *hineven
 * See LICENSE for licensing.
 */

#ifndef MI_HELPERS_H
#define MI_HELPERS_H

#include "core/common.h"
#include <rhi/rhi_desc.h>

MI_NAMESPACE_BEGIN

class RHIBuffer;
class RDGBuffer;
class RenderGraphBuilder;
class Helpers {
public:
    // Enqueue upload commands to the RHI graphics command queue and place barriers.
    // If you want that happen immediately, launch a submit on the queue and wait idle.
    static void Upload (RHIBufferSpan buffer, const void * data, size_t size) ;
    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDG (RenderGraphBuilder & builder, RHIBufferSpan buffer, const void * data, size_t size) ;
    // Add a RDG pass to upload data to a buffer.
    static void UploadWithRDG (RenderGraphBuilder & builder, RDGBuffer * buffer, const void * data, size_t size, size_t dst_offset = 0) ;
};

MI_NAMESPACE_END
#endif //MI_HELPERS_H
