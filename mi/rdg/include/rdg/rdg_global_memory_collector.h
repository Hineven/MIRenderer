/*
 * Created: 2025/12/26
 * Author:  hineven
 * See LICENSE for licensing.
 */
#pragma once

#include <vector>
#include <mutex>
#include <functional>
#include <utility>
#include "core/common.h"

MI_NAMESPACE_BEGIN

// A tiny global collector to hold one-time allocations done during
// shader/parameter reflection so they don't appear as leaks to tools.
class RDGGlobalMemoryCollector {
    static void EnsureCollector() ;
public:
    static RDGGlobalMemoryCollector & Get();

    RDGGlobalMemoryCollector(const RDGGlobalMemoryCollector&) = delete;
    RDGGlobalMemoryCollector& operator=(const RDGGlobalMemoryCollector&) = delete;

    // Allocate a single object and register it for cleanup at shutdown.
    template <class T, class... Args>
    T* New(Args&&... args) {
        T* p = new T(std::forward<Args>(args)...);
        Register(p, [](void* vp){ delete static_cast<T*>(vp); });
        return p;
    }

    // Allocate an array and register it for cleanup at shutdown.
    template <class T>
    T* NewArray(size_t n) {
        T* p = new T[n];
        Register(p, [](void* vp){ delete[] static_cast<T*>(vp); });
        return p;
    }

    // Register an externally allocated pointer with a custom deleter.
    void Register(void* ptr, std::function<void(void*)> deleter);

    ~RDGGlobalMemoryCollector();

private:
    RDGGlobalMemoryCollector() = default;

    struct Entry {
        void* ptr;
        std::function<void(void*)> deleter;
    };

    std::mutex mtx_;
    std::vector<Entry> entries_;
};

MI_NAMESPACE_END

