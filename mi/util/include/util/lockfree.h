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

template <typename T, LockFreeQueueUserType Producer, LockFreeQueueUserType Consumer, size_t RingBudget = 1024>
class TLockFreeQueue;

// Single producer, single consumer
template <typename T, size_t RingBudget>
class TLockFreeQueue<T, LockFreeQueueUserType::kOne, LockFreeQueueUserType::kOne, RingBudget> {
public:
    inline TLockFreeQueue() = default;

    FORCEINLINE bool Push (T&& t) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t next_head = (head + 1) % RingBudget;
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        ring_[head] = std::forward<T>(t);
        // Flush the ring_[head] = t write visible for all threads
        // before updating head_ using release semantics
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    FORCEINLINE bool Empty () const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    FORCEINLINE bool Pop (T& t) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        t = std::move(ring_[tail]);
        tail_.store((tail + 1) % RingBudget, std::memory_order_release);
        return true;
    }

private:
    T ring_[RingBudget];
    std::atomic<size_t> head_ {0};
    std::atomic<size_t> tail_ {0};
};

// Multiple producers, single consumer
template <typename T, size_t RingBudget>
class TLockFreeQueue<T, LockFreeQueueUserType::kMultiple, LockFreeQueueUserType::kOne, RingBudget> {
public:
    inline TLockFreeQueue() = default;
    
    FORCEINLINE bool Push (T&& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t head = head_ ++;
        size_t next_head = head % RingBudget;
        if (next_head == tail_.load(std::memory_order_acquire)) {
            head_ --;
            return false;
        }
        ring_[next_head] = std::forward<T>(t);
        return true;
    }

    FORCEINLINE bool Empty () const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    FORCEINLINE bool Pop (T& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        t = std::move(ring_[tail]);
        tail_.store((tail + 1) % RingBudget, std::memory_order_release);
        return true;
    }
private:

    // I don't know how to implement this, so just use mutex
    std::mutex mutex_;

    T ring_[RingBudget];
    std::atomic<size_t> head_ {0};
    std::atomic<size_t> tail_ {0};

};

MI_NAMESPACE_END

#endif //MIRENDERER_UTIL_LOCKFREE_H
