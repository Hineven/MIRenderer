/*
 * Created: 2025/5/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RADIX_SORT_H
#define RADIX_SORT_H

#include <rdg/rdg.h>

MI_NAMESPACE_BEGIN

class RadixSort {
public:
    static void AddRadixSort32BitsPass (
        RenderGraphBuilder & builder,
        uint32_t num_elements,
        RDGBuffer * src_keys_buffer,
        RDGBuffer * dst_keys_buffer,
        RDGBuffer * src_values_buffer = nullptr,
        RDGBuffer * dst_values_buffer = nullptr,
        // If the count_buffer is set, it will be used to store the count of each bin.
        // num bins will be a bound of this value in that case.
        RDGBuffer * count_buffer = nullptr,
        const std::string & name = ""
    ) ;

    constexpr static uint32_t kBitsPerPass = 8; // 8 bits per pass, 4 passes for 32 bits
    constexpr static uint32_t kBinsPerGroup = 1 << kBitsPerPass; // 256 bins per wave
    constexpr static uint32_t kElementsPerGroup = 1024; // process 1024 array elements per wave
};

MI_NAMESPACE_END

#endif //RADIX_SORT_H
