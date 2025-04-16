/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_HELPERS_H
#define MI_HELPERS_H
#include "core/common.h"
MI_NAMESPACE_BEGIN
class RHIBuffer;
class RDGBuffer;
class RenderGraphBuilder;
namespace Helpers {
    static void Upload (RHIBufferSpan buffer, const void * data, size_t size) ;
    static void UploadWithRDG (RenderGraphBuilder & builder, RHIBufferSpan buffer, const void * data, size_t size) ;
}

MI_NAMESPACE_END
#endif //MI_HELPERS_H
