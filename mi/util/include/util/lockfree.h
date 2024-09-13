/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_UTIL_LOCKFREE_H
#define MIRENDERER_UTIL_LOCKFREE_H

#include <concepts>
#include <type_traits>
#include <atomic>
#include <cassert>
#include "core/common.h"

MI_NAMESPACE_BEGIN

enum class LockFreeQueueUserType {
    kMultiple,
    kOne
};

// Multi producer, locked single consumer
template <typename T, LockFreeQueueUserType Producer, LockFreeQueueUserType Consumer, size_t RingBudget = 1024>
class TLockFreeQueue {
public:
    TLockFreeQueue() = default;

    bool Push (const T& t) {
        assert(false);
        return false;
    }

    bool Empty () const {
        assert(false);
        return false;
    }

    bool Pop (T& t) {
        assert(false);
        return false;
    }
};

MI_NAMESPACE_END

#endif //MIRENDERER_UTIL_LOCKFREE_H
