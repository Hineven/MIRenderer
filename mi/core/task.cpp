/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/task.h"
#include <chrono>

MI_NAMESPACE_BEGIN

thread_local TaskGraphThreadMeta G_ThreadMeta;

// Static members
TaskGraph * TaskGraph::instance_;

// Released after TaskGraph::instance_ is set.
static std::mutex instance_initialization_mutex_;

uint64_t GetCurrentTimeNanoseconds() {
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

void WorkerThreadRunnable::Run() {
    // Set thread local metadata
    G_ThreadMeta.index = thread_index_;
    G_ThreadMeta.performance = performance_type_;
    G_ThreadMeta.flags = 0;

    // Wait for the instance registration
    {
        instance_initialization_mutex_.lock();
        instance_initialization_mutex_.unlock();
    }

    SetCurrentThreadType(ThreadType::kTaskGraphWorkerThread);

    while (true) {
        Task *task = TaskGraph::Get().WaitAndGetNextTask(this);
        if (task != nullptr) {
            task->Run();
            // Finished, release the reference counter incremented by OnTaskReadyToRun.
            // The task can be destroyed after this point if no other references exist.
            task->DecRef();
        }
        if (stop_) break;
    }

    SetCurrentThreadType(ThreadType::kUnknown);
}

TaskInitializer::~TaskInitializer () {
    if (task_) {
        task_->Fire();
    }
}

TaskInitializer &TaskInitializer::operator=(TaskInitializer &&other) noexcept {
    if (this != &other) {
        // Fire current task if we have one
        if (task_) {
            task_->Fire();
        }
        task_graph_ = other.task_graph_;
        task_ = std::move(other.task_);
        other.task_ = nullptr;
    }
    return *this;
}


TaskRef TaskInitializer::Done(bool fire_immediately) {
    TaskRef result = task_;
    if (fire_immediately) {
        task_->Fire();
    }
    task_ = nullptr; // Transfer ownership
    return result;
}

void Task::Fire() {
    // The task must be created and fired in the same thread.
    assert(std::this_thread::get_id() == created_thread_id_);
    assert(state_ == TaskStateType::kUninitialized);

    std::lock_guard<std::mutex> lock(state_mutex_);
    state_ = TaskStateType::kReady;
    if (num_unfinished_precedents_ == 0) {
        TaskGraph::Get().OnTaskReadyToRun(this);
    }
}

void Task::OnPrecedentFinished() {
    if (--num_unfinished_precedents_ == 0) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == TaskStateType::kReady) {
            TaskGraph::Get().OnTaskReadyToRun(this);
        }
    }
}

void Task::Run() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = TaskStateType::kRunning;
    }

    try {
        task_function_();
        promise_.set_value();
    } catch (...) {
        promise_.set_exception(std::current_exception());
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = TaskStateType::kFinished;
    }

    // Notify successors
    for (auto& successor : successors_) {
        successor->OnPrecedentFinished();
    }

    task_graph_->OnTaskFinished(this);
}

bool Task::AddSuccessor(TaskRef successor) {
    if (state_ == TaskStateType::kUninitialized) {
        // Only the thread that creates the task can manipulate uninitialized tasks.
        assert(std::this_thread::get_id() == created_thread_id_);
        // No lock is needed because the task is not fired yet.
        successors_.push_back(successor);
        return true;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (state_ == TaskStateType::kFinished) return false;
    successors_.push_back(successor);
    return true;
}

Task::~Task () {

}

void Task::DeleteSelf() const {
    // Call destructor manually
    this->~Task();
    // Then free the memory through allocator
    if (task_graph_) {
        task_graph_->task_allocator_.Free(const_cast<Task*>(this));
    }
}


TaskGraphThreadMeta GetWorkerThreadMeta() {
    return G_ThreadMeta;
}

uint32_t GetWorkerThreadIndex() {
    return G_ThreadMeta.index;
}

TaskGraph::TaskGraph(int num_low_performance_threads, int num_high_performance_threads) {
    num_low_performance_threads_ = num_low_performance_threads;
    num_high_performance_threads_ = num_high_performance_threads;
    mi_assert(num_low_performance_threads_ + num_high_performance_threads_ <= C::kMaxTaskGraphThreadCount,
              "Too many threads");

    // Create worker runnables
    for (int i = 0; i < num_low_performance_threads; i++) {
        auto thread = std::make_unique<WorkerThreadRunnable>();
        thread->SetPerformanceType(ThreadPerformanceType::kLow);
        thread->SetThreadIndex(i);
        low_perf_thread_runnables_[i] = std::move(thread);
    }
    for (int i = 0; i < num_high_performance_threads; i++) {
        auto thread = std::make_unique<WorkerThreadRunnable>();
        thread->SetPerformanceType(ThreadPerformanceType::kHigh);
        thread->SetThreadIndex(i + num_low_performance_threads);
        high_perf_thread_runnables_[i] = std::move(thread);
    }

    // Launch threads using GetInfra()
    for (int i = 0; i < num_low_performance_threads; i++) {
        std::function<void()> wrapped_run = [this, i](){low_perf_thread_runnables_[i]->Run();};
        auto ret = GetInfra().LaunchThread(ThreadPerformanceType::kLow, wrapped_run);
        if (ret) {
            low_perf_threads_[i] = std::move(ret.value());
        } else {
            mi_assert(false, "Failed to launch %d th low performance thread", i);
        }
    }
    for (int i = 0; i < num_high_performance_threads; i++) {
        std::function<void()> wrapped_run = [this, i](){high_perf_thread_runnables_[i]->Run();};
        auto ret = GetInfra().LaunchThread(ThreadPerformanceType::kHigh, wrapped_run);
        if (ret) {
            high_perf_threads_[i] = std::move(ret.value());
        } else {
            mi_assert(false, "Failed to launch %d th high performance thread", i);
        }
    }
}

TaskGraph::~TaskGraph() {
    Shutdown();
}

void TaskGraph::OnTaskReadyToRun(Task* task) {
    if (shutdown_) return; // Don't accept new tasks if shutting down

    task->IncRef(); // Initialize reference count for task graph management
    {
        std::lock_guard<std::mutex> lock(task_queue_mutex_);
        if (!shutdown_) { // Double-check after acquiring lock
            task->state_ = TaskStateType::kPending;
            task_priority_queue_.push(task);
            pending_task_count_++;
        } else {
            task->DecRef();
            return;
        }
    }
    task_available_cv_.notify_one();
}

void TaskGraph::OnTaskFinished([[maybe_unused]] Task *task) {
    // This is called when a task finishes running
    // Do nothing really
}


Task* TaskGraph::WaitAndGetNextTask([[maybe_unused]] WorkerThreadRunnable* worker) {
    std::unique_lock<std::mutex> lock(task_queue_mutex_);

    // Wait until there's a task available or shutdown is requested
    task_available_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
        return !task_priority_queue_.empty() || shutdown_;
    });

    if (shutdown_ && task_priority_queue_.empty()) {
        return nullptr;
    }

    if (!task_priority_queue_.empty()) {
        Task* task = task_priority_queue_.top();
        task_priority_queue_.pop();
        pending_task_count_--;
        return task;
    }

    return nullptr;
}

TaskGraph& TaskGraph::Get() {
    if (!instance_) {
        mi_assert(false, "TaskGraph not initialized. Call InitializeSingleton first.");
    }
    return *instance_;
}

void TaskGraph::InitializeSingleton(int num_low_performance_threads, int num_high_performance_threads) {
    if (!instance_) {
        // Make sure TaskGraph::Get() is called only after initialization
        std::lock_guard<std::mutex> lock(instance_initialization_mutex_);
        instance_ = new TaskGraph(num_low_performance_threads, num_high_performance_threads);
    }
    // Make sure the write is visible to all threads
    std::atomic_thread_fence(std::memory_order_release);
}

void TaskGraph::DestroySingleton() {
    if (instance_) {
        instance_->Shutdown();
        delete instance_;
        instance_ = nullptr;
    }
}

void TaskGraph::Shutdown() {
    if (shutdown_) return;

    shutdown_ = true;

    // Stop all worker threads
    for (int i = 0; i < num_low_performance_threads_; i++) {
        if (low_perf_thread_runnables_[i]) {
            low_perf_thread_runnables_[i]->Stop();
        }
    }
    for (int i = 0; i < num_high_performance_threads_; i++) {
        if (high_perf_thread_runnables_[i]) {
            high_perf_thread_runnables_[i]->Stop();
        }
    }

    // Wake up all waiting threads
    task_available_cv_.notify_all();

    // Wait for all threads to finish
    for (int i = 0; i < num_low_performance_threads_; i++) {
        if (low_perf_threads_[i] && low_perf_threads_[i]->joinable()) {
            low_perf_threads_[i]->join();
        }
    }
    for (int i = 0; i < num_high_performance_threads_; i++) {
        if (high_perf_threads_[i] && high_perf_threads_[i]->joinable()) {
            high_perf_threads_[i]->join();
        }
    }
}

void TaskGraph::WaitForTask(TaskRef task) {
    if (task && task->GetState() != TaskStateType::kFinished) {
        task->GetFuture().wait();
    }
}

void TaskGraph::WaitForTasks(const std::vector<TaskRef>& tasks) {
    for (const auto& task : tasks) {
        WaitForTask(task);
    }
}

size_t TaskGraph::GetPendingTaskCount() const {
    return pending_task_count_;
}

MI_NAMESPACE_END
