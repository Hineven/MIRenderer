/*
 * Created: 2025/5/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RADIX_SORT_H
#define RADIX_SORT_H

#include <rdg/rdg.h>

MI_NAMESPACE_BEGIN

class DeviceRadixSort {
public:
    // Optimized for sorting up to millions of elements. Performance may degrade when sorting more / fewer elements.
    static void AddRadixSort32BitsPass (
        RenderGraphBuilder & builder,
        uint32_t num_elements, // Number of elements to sort if non-indirect. If indirect, this parameter stores an upper bound.
        RDGBuffer * src_keys_buffer,
        RDGBuffer * dst_keys_buffer,
        RDGBuffer * src_values_buffer = nullptr,
        RDGBuffer * dst_values_buffer = nullptr,
        // If the count_buffer is set, the sort will be performed in an indirect manner, where the actual number of elements to sort
        // is read from the count_buffer at execution time. The num_elements parameter will be treated as an upper bound.
        RDGBuffer * count_buffer = nullptr,
        const std::string & name = ""
    ) ;

    constexpr static uint32_t kBitsPerPass = 8; // 8 bits per pass, 4 passes for 32 bits. Must be consistent with the shader code.
    constexpr static uint32_t kBinsPerPass = 1 << kBitsPerPass; // 256 bins per thread group. Must be consistent with the shader code.
    constexpr static uint32_t kElementsPerSegment = 1024; // process 1024 array elements per thread group. Must be consistent with the shader code.
};

MI_NAMESPACE_END

#endif //RADIX_SORT_H
