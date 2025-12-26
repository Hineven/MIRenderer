/*
 * Created: 2025/12/26
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "rdg/rdg_global_memory_collector.h"
#include <cstdlib>
#include <mutex>

MI_NAMESPACE_BEGIN

static RDGGlobalMemoryCollector* g_collector = nullptr;
static std::once_flag g_init_flag;

void RDGGlobalMemoryCollector::EnsureCollector() {
    std::call_once(g_init_flag, [](){
        g_collector = new RDGGlobalMemoryCollector();
        // Register an atexit hook to release all kept allocations
        std::atexit([](){
            delete g_collector; // will release all entries in destructor
            g_collector = nullptr;
        });
    });
}

RDGGlobalMemoryCollector & RDGGlobalMemoryCollector::Get() {
    EnsureCollector();
    return *g_collector;
}

RDGGlobalMemoryCollector::~RDGGlobalMemoryCollector() {
    std::lock_guard<std::mutex> _{mtx_};
    for (auto & e : entries_) {
        if (e.ptr && e.deleter) e.deleter(e.ptr);
    }
    entries_.clear();
}

void RDGGlobalMemoryCollector::Register(void* ptr, std::function<void(void*)> deleter) {
    if (!ptr) return;
    EnsureCollector();
    std::lock_guard<std::mutex> _{mtx_};
    entries_.push_back({ptr, std::move(deleter)});
}

MI_NAMESPACE_END
