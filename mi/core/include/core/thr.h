/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RUNNABLE_H
#define MIRENDERER_RUNNABLE_H
#include "common.h"
#include "base.h"
#include "refcounted.h"
#include "util/lockfree.h"

MI_NAMESPACE_BEGIN

enum class ThreadType {
    kUnknown = 0,
    // Only 1 RHI thread.
    // It translates RHI commands to the underlying graphics API.
    kRHIThread = 1,
    // Only 1 render thread.
    // It is responsible for single-threaded logic on rendering the scene.
    // Only the render thread can access the RHI layer.
    kRenderThread = 2,
    // There can be 1 or more task graph worker threads.
    kTaskGraphWorkerThread = 3,
    Max
};

class ThreadRunnable : public NonCopyable, public NonMovable {
public:
    virtual void Run () = 0;
    virtual ~ThreadRunnable () = default;
};

ThreadType GetCurrentThreadType ();

FORCEINLINE bool IsRHIThread () {
    return GetCurrentThreadType() == ThreadType::kRHIThread;
}
FORCEINLINE bool IsRenderThread () {
    return GetCurrentThreadType() == ThreadType::kRenderThread;
}
FORCEINLINE bool IsTaskGraphWorkerThread () {
    return GetCurrentThreadType() == ThreadType::kTaskGraphWorkerThread;
}

void SetCurrentThreadType(ThreadType type);

MI_NAMESPACE_END
#endif //MIRENDERER_RUNNABLE_H
