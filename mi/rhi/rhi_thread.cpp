/*
 * Created: 2024/7/6
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <semaphore>
#include "rhi/rhi_thread.h"
#include "rhi/rhi_cmd.h"
#include "rhi_cmd_exec.h"
#include "rhi/rhi.h"

MI_NAMESPACE_BEGIN

enum class RHIThreadTaskType {
    kLambda,
    kTranslate,
    kSubmit
};

struct RHIThreadTask {
    union {
        void * ptr;
        struct {
            RHISyncPoint * sync_point;
            bool recycle;
        } submit;
    } param;
    std::function<void()> lambda;
    RHIThreadTaskType type;
    RHICommandQueueBase * queue;
    std::promise<void> promise;
};

// Task queue and relating semaphore
static TLockFreeQueue<RHIThreadTask, LockFreeQueueUserType::kMultiple, LockFreeQueueUserType::kOne> task_queue_;
static std::counting_semaphore<> task_queue_sem_ {0};

// The thread
static RHIWorkerThread * G_RHIWorkerThread = nullptr;

std::future<void> EnqueueRHICommandTranslationTask (RHICommandQueueBase * command_buffer, RHICommandBase * command_chain_head) {
    RHIThreadTask task;
    task.type = RHIThreadTaskType::kTranslate;
    task.queue = command_buffer;
    task.param.ptr = command_chain_head;
    auto future = task.promise.get_future();
    task_queue_.Push(std::move(task));
    task_queue_sem_.release();
    return future;
}

std::future<void> EnqueueRHICommandBufferSubmitTask (RHICommandQueueBase * command_buffer, RHISyncPoint * sync, bool recyle_resources) {
    RHIThreadTask task;
    task.type = RHIThreadTaskType::kSubmit;
    task.queue = command_buffer;
    task.param.submit.sync_point = sync;
    task.param.submit.recycle = recyle_resources;
    auto future = task.promise.get_future();
    task_queue_.Push(std::move(task));
    task_queue_sem_.release();
    return future;
}

void EnqueueRHIThreadIdleTask () {
    task_queue_sem_.release();
}

void StartAndRunRHIWorkerThread() {
    RHIWorkerThread * rhi_thread = GetInfra().New<RHIWorkerThread>();
    G_RHIWorkerThread = rhi_thread;
    rhi_thread->Run();
}

void SignalStopRHIWorkerThreads() {
    if(G_RHIWorkerThread) {
        G_RHIWorkerThread->SignalStop();
        EnqueueRHIThreadIdleTask();
    }
}

bool IsRHIThreadActive() {
    return G_RHIWorkerThread && G_RHIWorkerThread->IsRunning();
}

std::future<void> EnqueueRHIThreadTask(std::function<void()> && task) {
    RHIThreadTask rhi_task;
    rhi_task.type = RHIThreadTaskType::kLambda;
    rhi_task.queue = nullptr;
    rhi_task.lambda = std::move(task);
    auto future = rhi_task.promise.get_future();
    task_queue_.Push(std::move(rhi_task));
    task_queue_sem_.release();
    return future;
}

void AdvanceFrame_RHIThread() {
    mi_assert(IsRHIThread(), "AdvanceFrame_RHIThread must be called in RHI thread.");
    G_RHIWorkerThread->AdvanceFrame();
}

void RHIWorkerThread::Run() {

    if(GetCurrentThreadType() != ThreadType::kUnknown) {
        MI_LOG(MIInfraLogType::kError, "RHI thread is not created by an unknown thread.");
        return ;
    }
    SetCurrentThreadType(ThreadType::kRHIThread);


    static std::atomic<bool> rhi_thread_started {false};
    // Check if the thread has been started
    bool expected = false;
    rhi_thread_started.compare_exchange_strong(expected, true);
    if(expected) {
        MI_LOG(MIInfraLogType::kInfo, "There are more than one started RHI threads. Exiting.");
        return ;
    }
    is_running_ = true;
    MI_LOG(MIInfraLogType::kInfo, "RHI thread started.");
    while(!stop_signal_) {
        RHIThreadTask task;
        // Wait for at least one task
        task_queue_sem_.acquire();
        // Pop the task from the queue
        if(task_queue_.Pop(task)) {
            if(task.type == RHIThreadTaskType::kTranslate) {
                RHICommandBase *command = static_cast<RHICommandBase *>(task.param.ptr);
                while (command) {
                    command->ExecuteAndDestruct(*task.queue);
                    command = command->next_command_;
                }
                // Notify the task is finished
                task.promise.set_value();
            } else if(task.type == RHIThreadTaskType::kSubmit) {
                // Submit
                RHI::Get().GetCommandExecutor()->RHISubmitCommandBuffer(
                        task.queue,
                        task.param.submit.sync_point,
                        task.param.submit.recycle
                );
                // Notify the task is finished
                task.promise.set_value();
            } else if(task.type == RHIThreadTaskType::kLambda) {
                task.lambda();
                task.promise.set_value();
            } else {
                MI_LOG(MIInfraLogType::kError, "Unknown task type");
            }
        }
    }
    MI_LOG(MIInfraLogType::kInfo, "RHI thread stopping.");
    is_running_ = false;
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