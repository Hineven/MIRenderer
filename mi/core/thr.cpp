/*
 * Created: 2024/9/17
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <core/thr.h>

MI_NAMESPACE_BEGIN

thread_local ThreadType G_ThreadType = ThreadType::kUnknown;

void SetThreadType(ThreadType type) {
    G_ThreadType = type;
}

ThreadType GetThreadType() {
    return G_ThreadType;
}

MI_NAMESPACE_END