/*
 * Created: 2024/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_UTIL_LOCKFREE_H
#define MI_UTIL_LOCKFREE_H

#include <type_traits>
#include <atomic>
#include <cassert>
#include <mutex>
#include <thread>
#include <shared_mutex>
#include <vector>
#include <core/common.h>

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

    template<typename U = T>
    FORCEINLINE bool Push (U&& t) {
        static_assert(std::is_same_v<std::remove_cvref_t<U>, T>, "Push type must be the same as the queue type");
        size_t old_head = head_.load(std::memory_order_relaxed);
        size_t next_head = (old_head + 1) % RingBudget;
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        ring_[old_head] = std::forward<U>(t);
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
    T ring_[RingBudget] {};
    std::atomic<size_t> head_ {0};
    std::atomic<size_t> tail_ {0};
};

// Multiple producers, single consumer
template <typename T, size_t RingBudget>
class TLockFreeQueue<T, LockFreeQueueUserType::kMultiple, LockFreeQueueUserType::kOne, RingBudget> {
public:
    inline TLockFreeQueue() = default;

    template<typename U = T>
    FORCEINLINE void Push (U&& t) {
        static_assert(std::is_same_v<std::remove_cvref_t<U>, T>, "Push type must be the same as the queue type");
        while (true) {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t old_head = head_;
            size_t next_head = (old_head + 1) % RingBudget;
            if (next_head == tail_) {
                std::this_thread::yield();
            } else {
                ring_[old_head] = std::forward<U>(t);
                head_ = next_head;
                break;
            }
        }
    }

    FORCEINLINE bool Empty () const {
        return head_ == tail_;
    }

    FORCEINLINE bool Pop (T& t) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t tail = tail_;
        if (tail == head_) {
            return false;
        }
        t = std::move(ring_[tail]);
        tail_ = (tail + 1) % RingBudget;
        return true;
    }
private:

    // I don't know how to implement this, so just use mutex
    // TODO real lock-free implementation
    std::mutex mutex_ {};

    T ring_[RingBudget] {};
    std::atomic<size_t> head_ {0};
    std::atomic<size_t> tail_ {0};
};

// A queue designed to have multiple producers and one consumer relatively consuming all items in the queue.
// Writing to the queue frequently can potentially block the consumer.
template<typename T, uint32_t Budget = 16 * 1024>
class TConsumeAllQueue {
public:

    template<typename U = T>
    FORCEINLINE bool Push (U && element) {
        std::shared_lock lock(mutex_);
        size_t next_head = std::atomic_fetch_add(&head_, (size_t)1);
        if (next_head >= Budget) return false;
        data[next_head % Budget] = std::forward<U>(element);
        return true;
    }

    FORCEINLINE std::vector<T> ConsumeAll () {
        std::unique_lock lock(mutex_);
        size_t current_head = head_.load(std::memory_order_relaxed);
        size_t current_tail = 0;
        std::vector<T> result;
        result.reserve(current_head);

        while (current_tail < current_head) {
            result.emplace_back(std::move(data[current_tail % Budget]));
            current_tail++;
        }

        head_ = 0; // Reset head after consuming all
        return result;
    }

protected:
    T data[Budget];
    std::atomic<size_t> head_ {0};
    std::shared_mutex mutex_;
};

MI_NAMESPACE_END

#endif //MI_UTIL_LOCKFREE_H
