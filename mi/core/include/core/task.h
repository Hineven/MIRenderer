/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_TASK_H
#define MIRENDERER_TASK_H

#include <semaphore>
#include "core/common.h"
#include "core/thr.h"
#include "util/alloc.h"
#include "constants.h"

MI_NAMESPACE_BEGIN

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
private:
    // This has to be volatile because it's accessed by multiple threads.
    std::atomic<bool> stop_ {};

    ThreadPerformanceType performance_type_ {};
};

class Task;
typedef TRef<Task> TaskRef;

class Task : public RefCounted<> {
public:
    [[nodiscard]] inline std::future<void> GetFuture () {
        return task_.get_future();
    }

    // Add a successor to the task. Return true if the successor is added successfully.
    // Successors execute after the task finishes.
    // @return true if the successor is added successfully.
    bool AddSuccessor (TaskRef successor) ;

    // Fire the task. Convert its status from uninitialized to ready.
    void Fire () ;

    TaskStateType GetState () const {
        return state_;
    }

    friend class TaskGraph;
    friend class TaskInitializer;
    friend class WorkerThreadRunnable;
protected:
    void OnPrecedentFinished () ;

    void Run () ;

    // Locked when state_ is being modified or successors_ is under use.
    std::mutex state_mutex_;

    std::atomic<int> num_unfinished_precedents_ {0};
    std::packaged_task<void()> task_;
    std::vector<TaskRef> successors_;

    // The thread used to create this task.
    uint32_t created_thread_id_ {0};

    // The state of the task.
    std::atomic<TaskStateType> state_ {TaskStateType::kUninitialized};
};

class TaskInitializer : public NonCopyable, public NonMovable {
public:
    inline TaskInitializer (TaskGraph & task_graph, Task * task) : task_graph_(task_graph), task_(task) {}
    inline TaskInitializer & DependsOn (TaskRef prev_task) {
        if(prev_task->AddSuccessor(task_))
            task_->num_unfinished_precedents_++;
        return *this;
    }
    // Return the task and mark it as ready to run
    inline TaskRef Done (bool fire_immediately = true) ;
    ~TaskInitializer () ;
private:
    TaskGraph & task_graph_;
    TaskRef task_ {};
};


class TaskGraph {
    friend class Task;
    friend class WorkerThreadRunnable;
    friend class TaskInitializer;
public:

    inline TaskInitializer CreateTask (std::function<void()> task_lambda) {
        auto task = task_allocator_.Allocate();
        task->task_ = std::packaged_task<void()>(task_lambda);
        OnTaskReadyToRun(task);
        return TaskInitializer(*this, task);
    }

    static TaskGraph & Get ();
    static void InitializeSingleton (int num_low_performance_threads, int num_high_performance_threads) ;

protected:
    inline TaskGraph (int num_low_performance_threads, int num_high_performance_threads) {
        num_low_performance_threads_ = num_low_performance_threads;
        num_high_performance_threads_ = num_high_performance_threads;
        mi_assert(num_low_performance_threads_ + num_high_performance_threads_ <= C::kMaxTaskGraphThreadCount,
                  "Too many threads");
        for (int i = 0; i < num_low_performance_threads; i++) {
            auto thread = std::make_unique<WorkerThreadRunnable>();
            thread->SetPerformanceType(ThreadPerformanceType::kLow);
            low_perf_thread_runnables_[i] = std::move(thread);
        }
        for (int i = 0; i < num_high_performance_threads; i++) {
            auto thread = std::make_unique<WorkerThreadRunnable>();
            thread->SetPerformanceType(ThreadPerformanceType::kHigh);
            high_perf_thread_runnables_[i] = std::move(thread);
        }
        // Kick off the threads
        for (int i = 0; i < num_low_performance_threads; i++) {
            std::function<void()> wrapped_run = [this, i](){low_perf_thread_runnables_[i]->Run();};
            auto ret = GetInfra().LaunchThread(ThreadPerformanceType::kLow, wrapped_run);
            if(ret) {
                low_perf_threads_[i] = std::move(ret.value());
            } else {
                mi_assert(false, "Failed to launch %d th low performance thread", i);
            }
        }
        for (int i = 0; i < num_high_performance_threads; i++) {
            std::function<void()> wrapped_run = [this, i](){high_perf_thread_runnables_[i]->Run();};
            auto ret = GetInfra().LaunchThread(ThreadPerformanceType::kHigh, wrapped_run);
            if(ret) {
                high_perf_threads_[i] = std::move(ret.value());
            } else {
                mi_assert(false, "Failed to launch %d th high performance thread", i);
            }
        }
    }

    inline ~TaskGraph() {
        for (int i = 0; i < num_low_performance_threads_; i++) {
            low_perf_thread_runnables_[i]->Stop();
        }
        for (int i = 0; i < num_high_performance_threads_; i++) {
            high_perf_thread_runnables_[i]->Stop();
        }
        task_semaphore_.release(num_low_performance_threads_ + num_high_performance_threads_);
        for (int i = 0; i < num_low_performance_threads_; i++) {
            low_perf_threads_[i]->join();
        }
        for (int i = 0; i < num_high_performance_threads_; i++) {
            high_perf_threads_[i]->join();
        }
    }

    inline void OnTaskReadyToRun (Task * task) {
        std::lock_guard<std::mutex> lock(task->state_mutex_);
        assert(task->state_ == TaskStateType::kReady && "Task is not ready to run");
        task->state_ = TaskStateType::kReady;
        task_queue_.Push(task);
        task_semaphore_.release();
    }

    // Wait and get the next task to run. Returns nullptr if no task is available.
    // Invoked by the worker threads. Thread safe.
    Task * WaitAndGetNextTask (WorkerThreadRunnable * worker) ;

    TFixedElementAllocator<Task, C::kMaxTaskGraphTaskCount> task_allocator_;
    TLockFreeQueue<Task*, LockFreeQueueUserType::kMultiple, LockFreeQueueUserType::kOne> task_queue_;

    // Worker Runnables
    int num_low_performance_threads_ {};
    std::unique_ptr<WorkerThreadRunnable> low_perf_thread_runnables_[C::kMaxTaskGraphThreadCount];
    int num_high_performance_threads_ {};
    std::unique_ptr<WorkerThreadRunnable> high_perf_thread_runnables_[C::kMaxTaskGraphThreadCount];

    // Threads
    std::unique_ptr<std::thread> low_perf_threads_[C::kMaxTaskGraphThreadCount];
    std::unique_ptr<std::thread> high_perf_threads_[C::kMaxTaskGraphThreadCount];

    // Semaphore for tasks, incremented when a task is added to the queue.
    std::counting_semaphore<0> task_semaphore_ {0};
};

struct TaskGraphThreadMeta {
    uint32_t flags : 16;
    uint32_t index : 8;
    ThreadPerformanceType performance : 8;
};

// Get the metadata stored within the thread local storage
// Returns performace == kMax if the thread is not initialized.
TaskGraphThreadMeta GetThreadMeta ();

uint32_t GetThreadID ();

MI_NAMESPACE_END

#endif //MIRENDERER_TASK_H
