/*
 * Created: 2024/7/6
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <semaphore>
#include "rhi/rhi_worker.h"
#include "rhi/rhi_cmd.h"
#include "rhi_cmd_exec.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

enum class RHIThreadTaskType {
    kTranslate,
    kSubmit
};

struct RHIThreadTask {
    RHIThreadTaskType type;
    RHICommandQueueBase * queue;
    std::promise<void> promise;
    void * param;
};

// Task queue and relating semaphore
static TLockFreeQueue<RHIThreadTask, LockFreeQueueUserType::kMultiple, LockFreeQueueUserType::kOne> task_queue_;
static std::counting_semaphore<> task_queue_sem_ {0};

std::future<void> EnqueueRHICommandTranslationTask (RHICommandQueueBase * command_buffer, RHICommandBase * command_chain_head) {
    RHIThreadTask task;
    task.type = RHIThreadTaskType::kTranslate;
    task.queue = command_buffer;
    task.param = command_chain_head;
    auto future = task.promise.get_future();
    task_queue_.Push(task);
    task_queue_sem_.release();
    return future;
}

std::future<void> EnqueueRHICommandBufferSubmitTask (RHICommandQueueBase * command_buffer, bool wait_for_device_execution) {
    RHIThreadTask task;
    task.type = RHIThreadTaskType::kSubmit;
    task.queue = command_buffer;
    task.param = (void *)wait_for_device_execution;
    auto future = task.promise.get_future();
    task_queue_.Push(task);
    task_queue_sem_.release();
    return future;
}

void RHIWorkerThread::Run() {
    static std::atomic<bool> rhi_thread_started {false};
    // Check if the thread has been started
    if(!rhi_thread_started.exchange(true)) {
        MI_LOG(MIInfraLogType::kInfo, "There are more than one started RHI threads. Exiting.");
        return ;
    }
    while(!stop_signal_) {
        RHIThreadTask task;
        // Wait for at least one task
        task_queue_sem_.acquire();
        // Pop the task from the queue
        if(task_queue_.Pop(task)) {
            if(task.type == RHIThreadTaskType::kTranslate) {
                RHICommandBase *command = static_cast<RHICommandBase *>(task.param);
                while (command) {
                    command->ExecuteAndDestruct(*task.queue);
                    command = command->next_command_;
                }
                // Notify the task is finished
                task.promise.set_value();
            } else {
                // Submit
                RHI::Get().GetCommandExecutor()->RHISubmitCommandBuffer(task.queue, (bool)task.param);
                // Notify the task is finished
                task.promise.set_value();
            }
        }
    }
    // Reset the flag
    rhi_thread_started.store(false);
}

RHIWorkerThread::~RHIWorkerThread () {
    stop_signal_ = true;
    task_queue_sem_.release();
    // The thread may be running even after the destructor is called.
    // but we've set the stop signal, so it will exit soon.
}

MI_NAMESPACE_END