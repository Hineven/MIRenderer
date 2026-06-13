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

    InitializePlatformBackgroundThreadContext_Worker();

    while (true) {
        Task *task = TaskGraph::Get().WaitAndGetNextTask(this);
        if (task != nullptr) {
            if (!task->IsCancelled()) {
                task->Run();
            } else {
                // Task was cancelled — handle cleanup (notify successors, break promise)
                TaskGraph::Get().HandleCancelledTask(task);
            }
            // Release the reference counter incremented by OnTaskReadyToRun.
            // The task can be destroyed after this point if no other references exist.
            task->DecRef();
        }
        if (stop_) break;
    }

    DestroyPlatformBackgroundThreadContext_Worker();

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
    assert(state_ == TaskStateType::kUninitialized || state_ == TaskStateType::kCancelled);

    std::lock_guard<std::mutex> lock(state_mutex_);
    // If cancelled before firing, don't enqueue (promise was already broken by Cancel())
    if (cancelled_.load(std::memory_order_acquire)) return;
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
    if (state_ == TaskStateType::kFinished || state_ == TaskStateType::kCancelled) return false;
    successors_.push_back(successor);
    return true;
}

void Task::Cancel() {
    // Try to atomically claim the cancellation.
    // Only one thread wins the CAS — prevents double-cancel and cascade duplication.
    //
    // Promise cleanup responsibility:
    // - kUninitialized: Cancel() handles it (task never enters the set)
    // - kReady (CAS success): Cancel() handles it (task was never enqueued)
    // - kPending (CAS success): HandleCancelledTask handles it (task IS in the set,
    //   worker will dequeue and call HandleCancelledTask)
    TaskStateType expected = state_.load(std::memory_order_acquire);
    while (true) {
        if (expected == TaskStateType::kFinished || expected == TaskStateType::kCancelled) {
            return; // Already terminal — nothing to do
        }
        if (expected == TaskStateType::kUninitialized) {
            // Task not yet fired — set cancelled flag so Fire() will skip enqueue
            cancelled_.store(true, std::memory_order_release);
            state_ = TaskStateType::kCancelled;
            // Break promise so future waiters are released (task won't enter the set)
            try {
                promise_.set_exception(
                    std::make_exception_ptr(std::future_error(std::future_errc::broken_promise)));
            } catch (...) {}
            return;
        }
        // Try to transition from kReady or kPending to kCancelled
        TaskStateType prev = expected;
        if (state_.compare_exchange_strong(expected, TaskStateType::kCancelled,
                                            std::memory_order_acq_rel)) {
            cancelled_.store(true, std::memory_order_release);
            if (prev == TaskStateType::kReady) {
                // Task was fired but NOT yet enqueued (OnTaskReadyToRun hasn't run or
                // was blocked on state_mutex_ and will see cancelled_ and return early).
                // HandleCancelledTask will NOT be called for this task, so we must
                // break the promise here.
                try {
                    promise_.set_exception(
                        std::make_exception_ptr(std::future_error(std::future_errc::broken_promise)));
                } catch (...) {}
            }
            // If prev == kPending: task IS in the priority set. A worker will dequeue it,
            // see IsCancelled() == true, and call HandleCancelledTask which sets the promise
            // and notifies successors.
            return;
        }
        // CAS failed — expected was updated, retry with the new value
    }
}

void Task::UpdatePriority(TaskPriority new_priority) {
    if (!task_graph_) return;

    std::lock_guard<std::mutex> lock(task_graph_->task_queue_mutex_);
    auto it = task_graph_->task_set_.find(this);
    if (it != task_graph_->task_set_.end()) {
        task_graph_->task_set_.erase(it);
        priority_ = new_priority;
        task_graph_->task_set_.insert(this);
    } else {
        // Task is not in the queue (not yet enqueued, already dequeued, or finished)
        priority_ = new_priority;
    }
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
            task_set_.insert(task);
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

void TaskGraph::HandleCancelledTask(Task* task) {
    {
        std::lock_guard<std::mutex> lock(task->state_mutex_);
        // State should already be kCancelled (set by Cancel() or detected by worker)
        if (task->state_ != TaskStateType::kCancelled) {
            task->state_ = TaskStateType::kCancelled;
        }
    }

    // Set the future to broken_promise so waiters are released
    try {
        task->promise_.set_exception(
            std::make_exception_ptr(std::future_error(std::future_errc::broken_promise)));
    } catch (...) {
        // Promise might already be satisfied — ignore
    }

    // Notify successors: decrement their precedent counts so they can proceed
    for (auto& successor : task->successors_) {
        successor->OnPrecedentFinished();
    }
}

Task* TaskGraph::WaitAndGetNextTask([[maybe_unused]] WorkerThreadRunnable* worker) {
    std::unique_lock<std::mutex> lock(task_queue_mutex_);

    while (true) {
        // Wait until there's a task available or shutdown is requested
        task_available_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return !task_set_.empty() || shutdown_;
        });

        // Skip cancelled tasks at the top of the set
        while (!task_set_.empty()) {
            auto it = std::prev(task_set_.end());
            Task* top_task = *it;

            if (top_task->IsCancelled()) {
                task_set_.erase(it);
                pending_task_count_--;
                // Release queue's reference — task will be cleaned up by DecRef
                top_task->DecRef();
                continue;
            }

            // Found a non-cancelled task
            task_set_.erase(it);
            pending_task_count_--;
            return top_task;
        }

        // Set is empty after processing
        if (shutdown_) {
            return nullptr;
        }
    }
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
    if (task && task->GetState() != TaskStateType::kFinished
             && task->GetState() != TaskStateType::kCancelled) {
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

void TaskGraph::BatchEnqueue(const std::vector<TaskRef>& tasks) {
    // First, fire all tasks (sets state to kReady, no enqueue yet)
    for (const auto& task : tasks) {
        task->Fire();
    }

    // Then, enqueue all ready-to-run tasks under a single lock
    std::lock_guard<std::mutex> lock(task_queue_mutex_);
    if (shutdown_) return;

    for (const auto& task : tasks) {
        if (task->IsCancelled()) continue;
        if (task->state_.load() != TaskStateType::kReady) continue;
        if (task->num_unfinished_precedents_.load() > 0) continue;

        task->IncRef();
        task->state_ = TaskStateType::kPending;
        task_set_.insert(task.Raw());
        pending_task_count_++;
    }

    // Notify workers if we added any tasks
    if (pending_task_count_ > 0) {
        task_available_cv_.notify_all();
    }
}

MI_NAMESPACE_END
