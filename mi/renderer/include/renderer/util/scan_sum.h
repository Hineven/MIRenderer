/*
 * Created: 2025/10/1
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_SCAN_SUM_H
#define MI_SCAN_SUM_H
#include <core/common.h>
#include <rdg/rdg_base.h>

MI_NAMESPACE_BEGIN

class DeviceScanSum {
public:
    // Optimized for summing up to millions of elements. Performance may degrade when summing more / much fewer elements.
    // Inclusive sum
    static void AddScanSum32BitsPass (
        RenderGraphBuilder & builder,
        uint32_t num_elements, // Number of elements to sum if non-indirect. If indirect, this parameter stores an upper bound.
        RDGBuffer * src_values_buffer = nullptr,
        RDGBuffer * dst_values_buffer = nullptr,
        // If the count_buffer is set, the sum will be performed in an indirect manner, where the actual number of elements to sum
        // is read from the count_buffer at execution time. The num_elements parameter will be treated as an upper bound.
        RDGBuffer * count_buffer = nullptr,
        const std::string & name = ""
    );
    constexpr static uint32_t kElementsPerSegment = 1024; // process 1024 array elements per thread group. Must be consistent with the shader code.
    constexpr static uint32_t kThreadsPerGroup = 256; // Number of threads per thread group. Must be consistent with the shader code.
};

MI_NAMESPACE_END

#endif //MI_SCAN_SUM_H