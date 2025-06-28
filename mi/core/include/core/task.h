/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_TASK_H
#define MIRENDERER_TASK_H

#include <semaphore>
#include <queue>
#include <functional>
#include <future>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
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
    kMax
};

enum class TaskPriority : uint32_t {
    kLow = 0,
    kNormal = 1,
    kHigh = 2,
    kCritical = 3,
    kMax
};

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

    // Task priority for scheduling
    TaskPriority priority_ {TaskPriority::kNormal};

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

// Task comparator for priority queue (higher priority first, then FIFO for same priority)
struct TaskComparator {
    bool operator()(const Task* lhs, const Task* rhs) const {
        if (lhs->GetPriority() != rhs->GetPriority()) {
            return static_cast<uint32_t>(lhs->GetPriority()) < static_cast<uint32_t>(rhs->GetPriority());
        }
        // For same priority, earlier created tasks have higher priority (FIFO)
        return lhs->GetCreationTime() > rhs->GetCreationTime();
    }
};

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
    TaskRef CreateSimpleTask(Func&& func, TaskPriority priority = TaskPriority::kNormal) {
        return CreateTask(std::forward<Func>(func)).SetPriority(priority).Done();
    }

    template<typename Func>
    TaskRef CreateTaskWithDependency(Func&& func, TaskRef dependency, TaskPriority priority = TaskPriority::kNormal) {
        return CreateTask(std::forward<Func>(func)).DependsOn(dependency).SetPriority(priority).Done();
    }

    template<typename Func>
    TaskRef CreateTaskWithDependencies(Func&& func, const std::vector<TaskRef>& dependencies, TaskPriority priority = TaskPriority::kNormal) {
        auto initializer = std::move(CreateTask(std::forward<Func>(func)).SetPriority(priority));
        for (const auto& dep : dependencies) {
            initializer.DependsOn(dep);
        }
        return initializer.Done();
    }

    // Wait for a task to complete
    void WaitForTask(TaskRef task);

    // Wait for multiple tasks to complete
    void WaitForTasks(const std::vector<TaskRef>& tasks);

    void Shutdown();

    // Get number of pending tasks
    size_t GetPendingTaskCount() const;

protected:
    inline TaskGraph (int num_low_performance_threads, int num_high_performance_threads);

    inline ~TaskGraph();

    inline void OnTaskReadyToRun (Task * task);

    // Wait and get the next task to run. Returns nullptr if no task is available.
    // Invoked by the worker threads. Thread safe.
    Task * WaitAndGetNextTask (WorkerThreadRunnable * worker) ;

    // Called when a task finishes execution
    void OnTaskFinished(Task* task);

    // Use raw allocation - constructor/destructor not called by allocator
    TFixedElementAllocator<Task, C::kMaxTaskGraphTaskCount, 16, true, true> task_allocator_;

    // Priority queue for tasks (thread-safe)
    std::priority_queue<Task*, std::vector<Task*>, TaskComparator> task_priority_queue_;
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
    static std::mutex instance_mutex_;
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
