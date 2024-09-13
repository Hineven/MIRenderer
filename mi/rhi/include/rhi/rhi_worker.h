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
protected:
    volatile bool stop_signal_ {false};
};

// Signals all RHI worker threads to stop
void SignalStopRHIWorkerThreads ();

// Enqueue a command chain to the RHI threads for translation
std::future<void> EnqueueRHICommandTranslationTask (RHICommandQueueBase * command_buffer, RHICommandBase * command_chain_head) ;
// Enqueue a command buffer submission task for translated commands to the RHI threads
// If wait_for_device_execution is true, the task will wait for the device to finish executing the commands
std::future<void> EnqueueRHICommandBufferSubmitTask (RHICommandQueueBase * command_buffer, bool wait_for_device_execution = false) ;
MI_NAMESPACE_END

#endif //MIRENDERER_RHI_WORKER_H
