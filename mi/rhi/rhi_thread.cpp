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

#include "core/util/debug_prof.h"

MI_NAMESPACE_BEGIN

enum class RHIThreadTaskType {
    kLambda,
    kTranslate,
    kSubmit,
    kFrameEnd,
};

struct RHIThreadTask {
    union {
        void * ptr;
        struct {
            RHISyncPoint * sync_point;
            bool recycle;
        } submit;
        struct {
            RHISyncPoint * sync_point;
        } frame_end;
    } param;
#ifndef NDEBUG
    std::string name;
#endif
    std::function<void()> lambda;
    RHIThreadTaskType type;
    RHICommandQueueBase * queue;
    std::promise<void> promise;
};

// Task queue and relating semaphore
static TLockFreeQueue<RHIThreadTask, LockFreeQueueUserType::kMultiple, LockFreeQueueUserType::kOne> task_queue_ {};
static std::counting_semaphore<> task_queue_sem_ {0};

// The thread
static RHIWorkerThread * rhi_worker_thread_ = nullptr;

std::future<void> EnqueueRHICommandTranslationTask (RHICommandQueueBase * command_buffer, RHICommandBase * command_chain_head) {
    if (BYPASS_RHI_THREAD) {
        assert(IsRenderThread());
        // Do nothing actually
        auto promise = std::promise<void>();
        // Set the promise value to indicate the task is done
        promise.set_value();
        return promise.get_future();
    } else {
        RHIThreadTask task;
        task.type = RHIThreadTaskType::kTranslate;
        task.queue = command_buffer;
        task.param.ptr = command_chain_head;
        auto future = task.promise.get_future();
        task_queue_.Push(std::move(task));
        task_queue_sem_.release();
        return future;
    }
}

std::future<void> EnqueueRHICommandBufferSubmitTask (
    RHICommandQueueBase * command_buffer, RHISyncPoint * sync,
    const std::string & submit_prefix, bool recyle_resources
) {
    if (BYPASS_RHI_THREAD) {
        assert(IsRenderThread());
        RHI::Get().GetCommandExecutor()->RHISubmitCommandBuffer(command_buffer, sync, submit_prefix, recyle_resources);
        auto promise = std::promise<void>();
        // Set the promise value to indicate the task is done
        promise.set_value();
        return promise.get_future();
    } else {
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
}

std::future<void> EnqueueRHIFrameEndTask (RHICommandQueueBase * command_buffer, RHISyncPoint * sync) {
    if (BYPASS_RHI_THREAD) {
        assert(IsRenderThread());
        RHI::Get().GetCommandExecutor()->RHIFrameEnd(command_buffer, sync);
        auto promise = std::promise<void>();
        // Set the promise value to indicate the task is done
        promise.set_value();
        return promise.get_future();
    } else {
        RHIThreadTask task;
        task.type = RHIThreadTaskType::kFrameEnd;
        task.queue = command_buffer;
        task.param.frame_end.sync_point = sync;
        auto future = task.promise.get_future();
        task_queue_.Push(std::move(task));
        task_queue_sem_.release();
        return future;
    }
}

void EnqueueRHIThreadIdleTask () {
    task_queue_sem_.release();
}

void StartAndRunRHIWorkerThread() {
    RHIWorkerThread * rhi_thread = new RHIWorkerThread();
    rhi_worker_thread_ = rhi_thread;
    rhi_thread->Run();
}

void SignalStopRHIWorkerThreads() {
    if(rhi_worker_thread_) {
        rhi_worker_thread_->SignalStop();
        EnqueueRHIThreadIdleTask();
    }
}

bool IsRHIThreadActive() {
    return rhi_worker_thread_ && rhi_worker_thread_->IsRunning();
}

std::future<void> EnqueueRHIThreadTask(std::function<void()> && task) {
    if (BYPASS_RHI_THREAD) {
        task();
        auto promise = std::promise<void>();
        // Set the promise value to indicate the task is done
        promise.set_value();
        return promise.get_future();
    } else {
        RHIThreadTask rhi_task;
        rhi_task.type = RHIThreadTaskType::kLambda;
        rhi_task.queue = nullptr;
        rhi_task.lambda = std::move(task);
        auto future = rhi_task.promise.get_future();
        task_queue_.Push(std::move(rhi_task));
        task_queue_sem_.release();
        return future;
    }
}

void AdvanceFrame_RHIThread() {
    mi_assert(IsRHIThread() || BYPASS_RHI_THREAD, "AdvanceFrame_RHIThread must be called in RHI thread.");
    rhi_worker_thread_->AdvanceFrame();
}

void RHIWorkerThread::Run() {

    if (BYPASS_RHI_THREAD) {
        MI_LOG(MIInfraLogType::kWarning, "Bypassing RHI thread for debugging purposes. RHI thread exits upon its launch.");
        MI_LOG(MIInfraLogType::kWarning, "Set BYPASS_RHI_THREAD to false to silent this warning.");
        return ;
    }

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
        MI_LOG(MIInfraLogType::kWarning, "There are more than one started RHI threads. Exiting.");
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
                        task.name,
                        task.param.submit.recycle
                );
                // Notify the task is finished
                task.promise.set_value();
            } else if(task.type == RHIThreadTaskType::kLambda) {
                task.lambda();
                task.promise.set_value();
            } else if (task.type == RHIThreadTaskType::kFrameEnd) {
                // Frame end
                RHI::Get().GetCommandExecutor()->RHIFrameEnd(
                        task.queue,
                        task.param.frame_end.sync_point
                );
                // Notify the task is finished
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

size_t GetCurrentFrameIndex_RHIThread() {
    return rhi_worker_thread_->GetFrameIndex();
}


MI_NAMESPACE_END