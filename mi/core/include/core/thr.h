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
    // Only 1 render thread.
    // It is responsible for single-threaded logic on rendering the scene.
    // Only the render thread can access the RHI layer.
    kRenderThread = 1,
    // There can be 1 or more task graph worker threads.
    kTaskGraphWorkerThread = 2,
    Max
};

class ThreadRunnable : public NonCopyable, public NonMovable {
public:
    virtual void Run () = 0;
    virtual ~ThreadRunnable () = default;
};

ThreadType GetCurrentThreadType ();

FORCEINLINE bool IsRenderThread () {
    return GetCurrentThreadType() == ThreadType::kRenderThread;
}
FORCEINLINE bool IsTaskGraphWorkerThread () {
    return GetCurrentThreadType() == ThreadType::kTaskGraphWorkerThread;
}

void SetCurrentThreadType(ThreadType type);

MI_NAMESPACE_END
#endif //MIRENDERER_RUNNABLE_H
