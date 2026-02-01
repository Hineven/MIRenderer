/*
 * Created: 2026/2/1
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_UNORDERED_HASHING_H
#define MI_UNORDERED_HASHING_H

#include "core/common.h"
MI_NAMESPACE_BEGIN

// Compute a Zobrist hash for the given element.
// SImply XOR the returned value for each element to get a combined set hash.
uint32_t ZobristMap(const void * data, size_t size);

struct ZobristSetHashing {
    uint32_t Add(const void * data, size_t size) ;
    uint32_t GetResult() const;

    uint32_t hash_ {};
};

MI_NAMESPACE_END

#endif //MI_UNORDERED_HASHING_H