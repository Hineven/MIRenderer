/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/task.h"
MI_NAMESPACE_BEGIN

void WorkerThreadRunnable::Run() {
    while (true) {
        Task *task = TaskGraph::Get().WaitAndGetNextTask(this);
        if(task != nullptr) {
            task->Run();
            // Finished, release the reference counter incremented by TaskGraph.
            task->DecRef();
        }
        if(stop_) break;
    }
}

TaskInitializer::~TaskInitializer () {
    task_->Fire();
}

void Task::Fire() {
    // The task must be created and fired in the same thread.
    assert(GetThreadID() == created_thread_id_);
    assert(state_ == TaskStateType::kUninitialized);
    assert(num_unfinished_precedents_ == 0);
    state_ = TaskStateType::kReady;
    if(num_unfinished_precedents_ == 0) {
        TaskGraph::Get().OnTaskReadyToRun(this);
    }
}

void Task::OnPrecedentFinished () {
    if (--num_unfinished_precedents_ == 0) {
        TaskGraph::Get().OnTaskReadyToRun(this);
    }
}

void Task::Run () {
    task_();
    for (auto& successor : successors_) {
        successor->OnPrecedentFinished();
    }
}

bool Task::AddSuccessor (TaskRef successor) {
    if (state_ == TaskStateType::kUninitialized) {
        // Only the thread that creates the task can manipulate uninitialized tasks.
        assert(GetThreadID() == created_thread_id_);
        // No lock is needed because the task is not fired yet.
        successors_.push_back(successor);
        return true;
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ == TaskStateType::kFinished) return false;
    successors_.push_back(successor);
    return true;
}

thread_local TaskGraphThreadMeta G_ThreadMeta;

TaskGraphThreadMeta GetThreadMeta () {
    return G_ThreadMeta;
}

uint32_t GetThreadIndex () {
    return G_ThreadMeta.index;
}


Task *TaskGraph::WaitAndGetNextTask(mi::WorkerThreadRunnable *worker) {
    // Block until there is a task to run.
    task_semaphore_.release();
    // TODO actually schedule threads
    Task * ret = nullptr;
    bool has_task = task_queue_.Pop(ret);
    if (has_task) {
        return ret;
    }
    return nullptr;
}


MI_NAMESPACE_END
