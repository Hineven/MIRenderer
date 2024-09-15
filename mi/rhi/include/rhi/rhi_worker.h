/*
 * Created: 2024/7/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_RHI_WORKER_H
#define MIRENDERER_RHI_WORKER_H

#include "rhi/rhi_common.h"
#include "rhi/rhi_fwd.h"
#include "core/thr.h"

MI_NAMESPACE_BEGIN

class RHIWorkerThread : public ThreadRunnable {
public:
    void Run () ;
    inline void SignalStop () {
        stop_signal_ = true;
    }
    ~RHIWorkerThread() ;
protected:
    volatile bool stop_signal_ {false};
};

// Call to transit current thread to RHI worker thread
void StartAndRunRHIWorkerThread ();
// Signals all RHI worker threads to stop
void SignalStopRHIWorkerThreads ();

// Enqueue a command chain to the RHI threads for translation
// @return a future that will be ready when the translation is completed.
std::future<void> EnqueueRHICommandTranslationTask (RHICommandQueueBase * command_buffer, RHICommandBase * command_chain_head) ;
// Send flushed commands to RHI thread for baking and submission
// @param wait_for_device_execution if true, the returned future will wait
// for the device to finish executing the commands. Otherwise, it will only
// wait for the host submission completion.
// @return a future that will be ready when the submission/execution is completed.
std::future<void> EnqueueRHICommandBufferSubmitTask (RHICommandQueueBase * command_buffer, bool wait_for_device_execution = false) ;
MI_NAMESPACE_END

// Enqueue a task to the RHI thread for execution.
std::future<void> EnqueueRHIThreadTask (std::function<void()> task) ;

// Increment the frame counter kept by the RHI thread.
// The counter is used to filter RHI resources to recycle. Resources that are at least
// 1 frame older than the current frame will be recycled.
void AdvanceFrame_RHIThread () ;

#endif //MIRENDERER_RHI_WORKER_H
