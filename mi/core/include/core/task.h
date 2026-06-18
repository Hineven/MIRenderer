/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_TASK_H
#define MIRENDERER_TASK_H

#include <semaphore>
#include <queue>
#include <set>
#include <functional>
#include <future>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <climits>
#include "core/common.h"
#include "core/thr.h"
#include "core/fwd.h"
#include "util/alloc.h"
#include "constants.h"

MI_NAMESPACE_BEGIN

// Forward declaration
uint64_t GetCurrentTimeNanoseconds();

enum class TaskStateType {
    // A task can be quickly initialized within the thread that creates it.
    kUninitialized = 0,
    // When the task is fired it is converted to ready state.
    kReady,
    // The prequisites of the task is met and it has been pushed to the task queue pending for execution.
    kPending,
    // The task is being executed.
    kRunning,
    // The task has finished its execution. It will be destroyed once it's no longer referenced.
    kFinished,
    // The task was cancelled before execution. Its future is set to broken_promise.
    kCancelled,
    kMax
};

// Continuous priority type — higher value = higher priority.
// Supports fine-grained scheduling (e.g. distance-based priority for streaming).
using TaskPriority = uint32_t;

namespace TaskPriorities {
    constexpr TaskPriority kLow      = 0;
    constexpr TaskPriority kNormal   = UINT32_MAX / 4;
    constexpr TaskPriority kHigh     = UINT32_MAX / 2;
    constexpr TaskPriority kCritical = UINT32_MAX;
}

class WorkerThreadRunnable : public ThreadRunnable {
public:
    void Run () override;
    void Stop () {
        stop_ = true;
    }

    friend class TaskGraph;
protected:
    void SetPerformanceType (ThreadPerformanceType performance) {
        performance_type_ = performance;
    }

    void SetThreadIndex(uint32_t index) {
        thread_index_ = index;
    }

private:
    // This has to be volatile because it's accessed by multiple threads.
    std::atomic<bool> stop_ {};

    ThreadPerformanceType performance_type_ {};
    uint32_t thread_index_ {0};
};

class Task;
typedef TRef<Task> TaskRef;

class Task : public NonMovable, public NonCopyable {
public:
    // Reference counting methods for TRef compatibility
    inline uint32_t IncRef() const {
        return (uint32_t)atomic_fetch_add(&ref_count_, 1) + 1;
    }

    inline uint32_t DecRef() const {
        int ref_count = atomic_fetch_sub(&ref_count_, 1) - 1;
        if (ref_count == 0) {
            // Don't call delete this - use custom deletion for raw allocation
            DeleteSelf();
        }
        return (uint32_t)ref_count;
    }

    inline uint32_t GetRefCount() const {
        return (uint32_t)ref_count_;
    }

    [[nodiscard]] inline std::shared_future<void> GetFuture () {
        return future_;
    }

    // Add a successor to the task. Return true if the successor is added successfully.
    // Successors execute after the task finishes.
    // @return true if the successor is added successfully.
    bool AddSuccessor (TaskRef successor) ;

    // Fire the task. Convert its status from uninitialized to ready.
    void Fire () ;

    inline TaskStateType GetState () const {
        return state_;
    }

    inline TaskPriority GetPriority() const {
        return priority_;
    }

    inline void SetPriority(TaskPriority priority) {
        priority_ = priority;
    }

    // Dynamically update task priority. Thread-safe.
    // Removes the task from the priority set, updates the value, and re-inserts.
    void UpdatePriority(TaskPriority new_priority);

    // Cancel the task. Thread-safe.
    // A cancelled task is skipped by worker threads and its future is set to broken_promise.
    // Cascades to successors unless they have other unfinished precedents.
    void Cancel();

    inline bool IsCancelled() const {
        return cancelled_.load(std::memory_order_acquire);
    }

    // Get creation timestamp for scheduling
    inline uint64_t GetCreationTime() const {
        return creation_time_;
    }

    // Set the TaskGraph pointer for memory management
    inline void SetTaskGraph(TaskGraph* task_graph) {
        task_graph_ = task_graph;
    }

    friend class TaskGraph;
    friend class TaskInitializer;
    friend class WorkerThreadRunnable;

protected:
    inline Task() : creation_time_(0), task_graph_(nullptr) {
        creation_time_ = GetCurrentTimeNanoseconds();
        future_ = promise_.get_future().share();
    }

    ~Task () ;

    // Custom deletion method for raw allocation
    void DeleteSelf() const ;

    void OnPrecedentFinished () ;

    void Run () ;

    // Locked when state_ is being modified or successors_ is under use.
    std::mutex state_mutex_;

    std::atomic<int> num_unfinished_precedents_ {0};
    std::function<void()> task_function_;
    std::promise<void> promise_;
    std::shared_future<void> future_;
    std::vector<TaskRef> successors_;

    // Cancellation flag
    std::atomic<bool> cancelled_ {false};

    // Task priority for scheduling (higher value = higher priority)
    TaskPriority priority_ {TaskPriorities::kNormal};

    // Creation timestamp for FIFO within same priority
    uint64_t creation_time_;

    // The thread used to create this task.
    std::thread::id created_thread_id_ {};

    // The state of the task.
    std::atomic<TaskStateType> state_ {TaskStateType::kUninitialized};

    // Pointer to TaskGraph for memory management
    TaskGraph* task_graph_;

private:
    // Reference count for TRef compatibility
    mutable std::atomic<int> ref_count_ {0};
};

// Task comparator for priority set (higher priority first via rbegin, then FIFO for same priority)
struct TaskComparator {
    bool operator()(const Task* lhs, const Task* rhs) const {
        if (lhs->GetPriority() != rhs->GetPriority()) {
            return lhs->GetPriority() < rhs->GetPriority();
        }
        // For same priority, earlier created tasks have higher priority (FIFO)
        return lhs->GetCreationTime() > rhs->GetCreationTime();
    }
};

// Lightweight cooperative cancellation token.
// Can be shared among multiple tasks (via TRef) to allow external cancellation.
// Unlike Task::Cancel() which skips queued tasks, the token lets running tasks
// check cancellation and exit early. Uses the project's RefCounted<> + TRef
// system (thread-safe, unified lifetime control) rather than std::shared_ptr.
class CancellationToken : public RefCounted<> {
public:
    void Cancel() { cancelled_.store(true, std::memory_order_release); }
    bool IsCancelled() const { return cancelled_.load(std::memory_order_acquire); }
private:
    std::atomic<bool> cancelled_{false};
};
using CancellationTokenRef = TRef<CancellationToken>;

class TaskInitializer : public NonCopyable {
public:
    inline TaskInitializer (TaskGraph * task_graph, TaskRef task) : task_graph_(task_graph), task_(std::move(task)) {}

    // Move constructor
    inline TaskInitializer(TaskInitializer&& other) noexcept
        : task_graph_(other.task_graph_), task_(std::move(other.task_)) {
        other.task_ = nullptr; // Ensure other doesn't fire in destructor
    }

    // Move assignment
    TaskInitializer& operator=(TaskInitializer&& other) noexcept ;

    inline TaskInitializer & DependsOn (const TaskRef& prev_task) {
        if(prev_task->AddSuccessor(task_))
            task_->num_unfinished_precedents_++;
        return *this;
    }

    inline TaskInitializer & SetPriority(TaskPriority priority) {
        task_->SetPriority(priority);
        return *this;
    }

    // Return the task and mark it as ready to run
    TaskRef Done (bool fire_immediately = true) ;
    ~TaskInitializer () ;

private:
    TaskGraph * task_graph_;
    TaskRef task_ {};
};

// A group of related tasks that can be cancelled or waited on together.
// Typical use: one TaskGroup per streaming chunk, holding all its pipeline tasks.
class TaskGroup : public RefCounted<> {
public:
    TaskGroup() : token_(mi::Create<CancellationToken>()) {}

    void Add(TaskRef task) {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }

    // Cancel all tasks in the group + set the cooperative cancellation token.
    void CancelAll() {
        token_->Cancel();
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& task : tasks_) {
            if (task && task->GetState() != TaskStateType::kFinished
                     && task->GetState() != TaskStateType::kCancelled) {
                task->Cancel();
            }
        }
    }

    void WaitAll() {
        std::vector<TaskRef> tasks_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_copy = tasks_;
        }
        for (auto& task : tasks_copy) {
            if (task) {
                task->GetFuture().wait();
            }
        }
    }

    void SetPriorityAll(TaskPriority priority) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& task : tasks_) {
            if (task && task->GetState() != TaskStateType::kFinished
                     && task->GetState() != TaskStateType::kCancelled) {
                task->UpdatePriority(priority);
            }
        }
    }

    CancellationTokenRef GetToken() const { return token_; }

    size_t GetTaskCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<TaskRef> tasks_;
    CancellationTokenRef token_;
};

using TaskGroupRef = TRef<TaskGroup>;

class TaskGraph {
    friend class Task;
    friend class WorkerThreadRunnable;
    friend class TaskInitializer;
public:

    template<typename Func>
    inline TaskInitializer CreateTask (Func&& task_lambda) {
        auto task = task_allocator_.Allocate();
        new (task) Task(); // Placement new to call constructor
        task->task_function_ = std::forward<Func>(task_lambda);
        task->created_thread_id_ = std::this_thread::get_id();
        task->SetTaskGraph(this); // Set TaskGraph pointer for memory management
        return {this, TaskRef(task)};
    }

    static TaskGraph & Get ();
    static void InitializeSingleton (int num_low_performance_threads, int num_high_performance_threads) ;
    static void DestroySingleton () ;

    // Helper functions for easy task creation and dependency management
    template<typename Func>
    TaskRef CreateSimpleTask(Func&& func, TaskPriority priority = TaskPriorities::kNormal) {
        return CreateTask(std::forward<Func>(func)).SetPriority(priority).Done();
    }

    template<typename Func>
    TaskRef CreateTaskWithDependency(Func&& func, TaskRef dependency, TaskPriority priority = TaskPriorities::kNormal) {
        return CreateTask(std::forward<Func>(func)).DependsOn(dependency).SetPriority(priority).Done();
    }

    template<typename Func>
    TaskRef CreateTaskWithDependencies(Func&& func, const std::vector<TaskRef>& dependencies, TaskPriority priority = TaskPriorities::kNormal) {
        auto initializer = std::move(CreateTask(std::forward<Func>(func)).SetPriority(priority));
        for (const auto& dep : dependencies) {
            initializer.DependsOn(dep);
        }
        return initializer.Done();
    }

    // Wait for a task to complete (handles finished and cancelled states)
    void WaitForTask(TaskRef task);

    // Wait for multiple tasks to complete
    void WaitForTasks(const std::vector<TaskRef>& tasks);

    void Shutdown();

    // Get number of pending tasks (includes cancelled tasks still in the set)
    size_t GetPendingTaskCount() const;

    // Batch enqueue multiple tasks. All tasks are fired and enqueued under a single lock,
    // reducing lock contention compared to individual Done() calls.
    // Tasks must not have been fired yet (state must be kUninitialized).
    void BatchEnqueue(const std::vector<TaskRef>& tasks);

    // Shortcut for simple parallization
    template<typename T, typename F>
    inline std::vector<TaskRef> ForEach(T & iteratable, F runnable) {
        std::vector<TaskRef> tasks;
        for (auto & e : iteratable) tasks.push_back(CreateSimpleTask([&](){runnable(e);}));
        return std::move(tasks);
    }

    // Shortcut for simple parallization with blocked ranges
    // num_threads: number of threads to use. 0 means automatic. -x means use {maxNumUsableThreads - x} threads.
    template<typename RangeType, typename F>
    inline std::vector<TaskRef> ForEachBlockedRange(RangeType begin, RangeType end, F runnable, int num_threads = 0) {
        bool direction = end > begin;
        RangeType total_size = direction ? (end - begin) : (begin - end);
        std::vector<TaskRef> tasks;
        if (num_threads == 0) num_threads = MaxNumTaskThreads();
        else if (num_threads < 0) num_threads = std::max(1, MaxNumTaskThreads() + num_threads);
        RangeType block_size = (total_size + (RangeType)(num_threads - 1)) / (RangeType)num_threads;
        for (int i = 0; i < num_threads; i++) {
            RangeType block_begin = direction ? (begin + i * block_size) : (begin - i * block_size);
            RangeType block_end = std::clamp(direction ? block_begin + block_size : block_begin - block_size, begin, end);
            tasks.push_back(CreateSimpleTask([block_begin, block_end, runnable]() {runnable(block_begin, block_end);}));
        }
        return std::move(tasks);
    }

    inline int MaxNumTaskThreads() const {
        return num_low_performance_threads_ + num_high_performance_threads_;
    }

protected:
    inline TaskGraph (int num_low_performance_threads, int num_high_performance_threads);

    inline ~TaskGraph();

    inline void OnTaskReadyToRun (Task * task);

    // Wait and get the next task to run. Returns nullptr if no task is available.
    // Invoked by the worker threads. Thread safe.
    // Cancelled tasks are handled internally (skipped and cleaned up).
    Task * WaitAndGetNextTask (WorkerThreadRunnable * worker) ;

    // Called when a task finishes execution
    void OnTaskFinished(Task* task);

    // Handle a cancelled task: set state, break promise, notify successors.
    // Called by worker thread after dequeuing a cancelled task.
    void HandleCancelledTask(Task* task);

    // Use raw allocation - constructor/destructor not called by allocator
    TFixedElementAllocator<Task, C::kMaxTaskGraphTaskCount, 16, true, true> task_allocator_;

    // Priority set for tasks (thread-safe). Use rbegin() for highest priority.
    std::set<Task*, TaskComparator> task_set_;
    std::mutex task_queue_mutex_;
    std::condition_variable task_available_cv_;
    std::atomic<size_t> pending_task_count_{0};

    // Worker Runnables
    int num_low_performance_threads_ {};
    std::unique_ptr<WorkerThreadRunnable> low_perf_thread_runnables_[C::kMaxTaskGraphThreadCount];
    int num_high_performance_threads_ {};
    std::unique_ptr<WorkerThreadRunnable> high_perf_thread_runnables_[C::kMaxTaskGraphThreadCount];

    // Threads
    std::unique_ptr<std::thread> low_perf_threads_[C::kMaxTaskGraphThreadCount];
    std::unique_ptr<std::thread> high_perf_threads_[C::kMaxTaskGraphThreadCount];

    // Shutdown flag
    std::atomic<bool> shutdown_{false};

private:
    static TaskGraph * instance_;
};

// Helper for batch task submission. Collects unfired tasks and submits them
// to the TaskGraph under a single lock for minimal contention.
//
// Usage:
//   TaskBatch batch;
//   batch.Add(task_graph.CreateTask([](){...}).SetPriority(p));
//   batch.Add(task_graph.CreateTask([](){...}).DependsOn(other).SetPriority(p));
//   batch.Submit();
class TaskBatch {
public:
    TaskBatch() = default;

    // Move only
    TaskBatch(TaskBatch&& other) noexcept : tasks_(std::move(other.tasks_)) {}
    TaskBatch& operator=(TaskBatch&& other) noexcept {
        tasks_ = std::move(other.tasks_);
        return *this;
    }
    TaskBatch(const TaskBatch&) = delete;
    TaskBatch& operator=(const TaskBatch&) = delete;

    // Fire the task (via Done()) and add to batch. Returns the fired TaskRef.
    TaskRef Add(TaskInitializer&& init) {
        TaskRef task = init.Done(true);
        tasks_.push_back(task);
        return task;
    }

    // Submit all collected tasks to the TaskGraph with a single lock acquisition.
    void Submit() {
        if (!tasks_.empty()) {
            TaskGraph::Get().BatchEnqueue(tasks_);
            tasks_.clear();
        }
    }

    size_t Size() const { return tasks_.size(); }

private:
    std::vector<TaskRef> tasks_;
};

struct TaskGraphThreadMeta {
    uint32_t flags : 16;
    uint32_t index : 8;
    ThreadPerformanceType performance : 8;
};

// Get the metadata stored within the thread local storage
// Returns performace == kMax if the thread is not initialized.
TaskGraphThreadMeta GetWorkerThreadMeta ();

// Get the thread index for the worker thread within the thread pool.
uint32_t GetWorkerThreadIndex ();

// Helper function to get current time in nanoseconds
uint64_t GetCurrentTimeNanoseconds();

MI_NAMESPACE_END

#endif //MIRENDERER_TASK_H
