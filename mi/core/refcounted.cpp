/*
 * Created: 2025/12/25
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <atomic>
#include <core/common.h>
MI_NAMESPACE_BEGIN

#ifndef NDEBUG

static std::atomic<uint32_t> g_refcounted_object_count = 0;

uint32_t GetRefCountedObjectCount() {
    return g_refcounted_object_count.load();
}

void IncrementRefCountedObjectCount() {
    g_refcounted_object_count.fetch_add(1);
}

void DecrementRefCountedObjectCount() {
    g_refcounted_object_count.fetch_sub(1);
}

#endif

MI_NAMESPACE_END