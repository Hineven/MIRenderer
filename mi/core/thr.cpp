/*
 * Created: 2024/9/17
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <core/thr.h>

MI_NAMESPACE_BEGIN

thread_local ThreadType G_ThreadType = ThreadType::kUnknown;

void SetCurrentThreadType(ThreadType type) {
    G_ThreadType = type;
}

ThreadType GetCurrentThreadType() {
    return G_ThreadType;
}

MI_NAMESPACE_END