/*
 * Author:  hineven
 * See LICENSE for licensing.
 * Created: 2024/7/5
 */

#ifndef MI_RHI_THREAD_H
#define MI_RHI_THREAD_H

#include <future>
#include "rhi/rhi_common.h"
#include "rhi/rhi_fwd.h"
#include "core/thr.h"

 MI_NAMESPACE_BEGIN

class RHIWorkerThread : public ThreadRunnable {
public:
    void Run () ;
    FORCEINLINE void SignalStop () {
        stop_signal_ = true;
    }
    FORCEINLINE size_t GetFrameIndex () const {
        return frame_index_;
    }
    FORCEINLINE void AdvanceFrame () {
        frame_index_++;
    }
    FORCEINLINE bool IsRunning () const {
        return is_running_;
    }
    ~RHIWorkerThread() ;
protected:
    std::atomic<size_t> frame_index_ {0};
    volatile bool stop_signal_ {false};
    volatile bool is_running_ {false};
};

// Call to transit current thread to RHI worker thread
void StartAndRunRHIWorkerThread ();
// Signals all (currently only one) RHI worker threads to stop
void SignalStopRHIWorkerThreads ();

// If there is an active RHI thread
bool IsRHIThreadActive () ;


// Enqueue a command chain to the RHI threads for translation
// @return a future that will be ready when the translation is completed.
std::future<void> EnqueueRHICommandTranslationTask (RHICommandQueueBase * command_buffer, RHICommandBase * command_chain_head) ;
// Send flushed commands to RHI thread for baking and submission
// @param sync a sync point that can be waited on for the device to complete executing the submitted commands.
// @param recyle_resources whether to recycle translated commands immediately after submission rather than in
// frame intervals. May cause overhead.
// @return a future that will be ready when the submission is completed.
std::future<void> EnqueueRHICommandBufferSubmitTask (RHICommandQueueBase * command_buffer, RHISyncPoint * sync, bool recyle_resources = false) ;

// Enqueue a task to the RHI thread for execution.
// @return a future that will be ready when the task is completed on RHI thread.
std::future<void> EnqueueRHIThreadTask (std::function<void()> && task) ;

// Invoke the RHI thread to do 1 loop.
void EnqueueRHIThreadIdleTask ();

// Increment the frame counter kept by the RHI thread.
// The counter is used to filter RHI resources to recycle. Resources that are at least
// 1 frame older than the current frame will be recycled.
void AdvanceFrame_RHIThread () ;

MI_NAMESPACE_END
#endif //MI_RHI_THREAD_H
