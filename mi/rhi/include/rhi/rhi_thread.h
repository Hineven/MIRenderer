/*
 * Created: 2024/7/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RHI_THREAD_H
#define MI_RHI_THREAD_H

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
// @param wait_for_device_execution if true, the returned future will wait
// for the device to finish executing the commands. Otherwise, it will only
// wait for the host submission completion.
// @return a future that will be ready when the submission/execution is completed.
std::future<void> EnqueueRHICommandBufferSubmitTask (RHICommandQueueBase * command_buffer, bool wait_for_device_execution = false) ;

// Enqueue a task to the RHI thread for execution.
std::future<void> EnqueueRHIThreadTask (std::function<void()> && task) ;

// Increment the frame counter kept by the RHI thread.
// The counter is used to filter RHI resources to recycle. Resources that are at least
// 1 frame older than the current frame will be recycled.
void AdvanceFrame_RHIThread () ;

MI_NAMESPACE_END
#endif //MI_RHI_THREAD_H
