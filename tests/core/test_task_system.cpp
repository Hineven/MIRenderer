/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include "core/task.h"
#include <infra_impl/infra.h>

using namespace mi;

class TaskSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        using namespace mi;
        TransferInfra(std::make_unique<MyInfra>(true));
        GetInfra().Init();

        // Initialize task system with 2 low-perf and 2 high-perf threads
        TaskGraph::InitializeSingleton(2, 2);
    }

    void TearDown() override {
        TaskGraph::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    }
};

// ============================================================================
// Existing tests (updated for new TaskPriorities namespace)
// ============================================================================

TEST_F(TaskSystemTest, BasicTaskExecution) {
    std::atomic<int> counter{0};

    // Create a simple task
    auto task = TaskGraph::Get().CreateSimpleTask([&counter]() {
        counter.store(42);
    });

    // Wait for task completion
    TaskGraph::Get().WaitForTask(task);

    EXPECT_EQ(counter.load(), 42);
    EXPECT_EQ(task->GetState(), TaskStateType::kFinished);
}

TEST_F(TaskSystemTest, TaskPriority) {
    std::vector<int> execution_order;
    std::mutex order_mutex;

    // Create tasks with different priorities
    auto low_task = TaskGraph::Get().CreateSimpleTask([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(0); // Low priority
    }, TaskPriorities::kLow);

    auto high_task = TaskGraph::Get().CreateSimpleTask([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(2); // High priority
    }, TaskPriorities::kHigh);

    auto normal_task = TaskGraph::Get().CreateSimpleTask([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(1); // Normal priority
    }, TaskPriorities::kNormal);

    // Wait for all tasks
    TaskGraph::Get().WaitForTasks({low_task, high_task, normal_task});

    // Verify all tasks completed
    EXPECT_EQ(low_task->GetState(), TaskStateType::kFinished);
    EXPECT_EQ(high_task->GetState(), TaskStateType::kFinished);
    EXPECT_EQ(normal_task->GetState(), TaskStateType::kFinished);

    // High priority should execute first
    EXPECT_EQ(execution_order[0], 2);
}

TEST_F(TaskSystemTest, TaskDependencies) {
    std::atomic<int> step{0};

    // Create a chain of dependent tasks
    auto task1 = TaskGraph::Get().CreateSimpleTask([&step]() {
        EXPECT_EQ(step.load(), 0);
        step.store(1);
    });

    auto task2 = TaskGraph::Get().CreateTaskWithDependency([&step]() {
        EXPECT_EQ(step.load(), 1);
        step.store(2);
    }, task1);

    auto task3 = TaskGraph::Get().CreateTaskWithDependency([&step]() {
        EXPECT_EQ(step.load(), 2);
        step.store(3);
    }, task2);

    // Wait for the final task
    TaskGraph::Get().WaitForTask(task3);

    EXPECT_EQ(step.load(), 3);
    EXPECT_EQ(task1->GetState(), TaskStateType::kFinished);
    EXPECT_EQ(task2->GetState(), TaskStateType::kFinished);
    EXPECT_EQ(task3->GetState(), TaskStateType::kFinished);
}

TEST_F(TaskSystemTest, MultipleTaskDependencies) {
    std::atomic<int> completed_deps{0};

    // Create multiple dependency tasks
    auto dep1 = TaskGraph::Get().CreateSimpleTask([&completed_deps]() {
        completed_deps.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });

    auto dep2 = TaskGraph::Get().CreateSimpleTask([&completed_deps]() {
        completed_deps.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });

    auto dep3 = TaskGraph::Get().CreateSimpleTask([&completed_deps]() {
        completed_deps.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });

    // Create a task that depends on all three
    auto final_task = TaskGraph::Get().CreateTaskWithDependencies([&completed_deps]() {
        EXPECT_EQ(completed_deps.load(), 3);
        completed_deps.store(100);
    }, {dep1, dep2, dep3});

    TaskGraph::Get().WaitForTask(final_task);

    EXPECT_EQ(completed_deps.load(), 100);
}

TEST_F(TaskSystemTest, TaskWithFuture) {
    auto task = TaskGraph::Get().CreateSimpleTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    auto future = task->GetFuture();

    // Future should not be ready immediately
    EXPECT_EQ(future.wait_for(std::chrono::milliseconds(1)), std::future_status::timeout);

    // Wait for completion
    future.wait();

    EXPECT_EQ(task->GetState(), TaskStateType::kFinished);
}

TEST_F(TaskSystemTest, TaskExceptionHandling) {
    auto task = TaskGraph::Get().CreateSimpleTask([]() {
        throw std::runtime_error("Test exception");
    });

    auto future = task->GetFuture();

    // Wait for task completion
    TaskGraph::Get().WaitForTask(task);

    // Task should be finished even with exception
    EXPECT_EQ(task->GetState(), TaskStateType::kFinished);

    // Future should contain the exception
    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST_F(TaskSystemTest, ConcurrentTaskExecution) {
    const int num_tasks = 100;
    std::atomic<int> counter{0};
    std::vector<TaskRef> tasks;

    // Create many concurrent tasks
    for (int i = 0; i < num_tasks; ++i) {
        auto task = TaskGraph::Get().CreateSimpleTask([&counter]() {
            counter.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
        tasks.push_back(task);
    }

    // Wait for all tasks
    TaskGraph::Get().WaitForTasks(tasks);

    EXPECT_EQ(counter.load(), num_tasks);

    // All tasks should be finished
    for (const auto& task : tasks) {
        EXPECT_EQ(task->GetState(), TaskStateType::kFinished);
    }
}

TEST_F(TaskSystemTest, TaskCreationSyntax) {
    std::atomic<bool> executed{false};

    // Test fluent interface syntax
    auto task = TaskGraph::Get()
        .CreateTask([&executed]() {
            executed.store(true);
        })
        .SetPriority(TaskPriorities::kHigh)
        .Done();

    TaskGraph::Get().WaitForTask(task);

    EXPECT_TRUE(executed.load());
    EXPECT_EQ(task->GetPriority(), TaskPriorities::kHigh);
}

TEST_F(TaskSystemTest, TaskGraphMetrics) {
    // Initially no pending tasks
    EXPECT_EQ(TaskGraph::Get().GetPendingTaskCount(), 0);

    // Create a long-running task
    auto task = TaskGraph::Get().CreateSimpleTask([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });

    // Should have pending tasks briefly
    // Note: This might be racy, but gives us basic verification

    TaskGraph::Get().WaitForTask(task);

    // After completion, no pending tasks
    EXPECT_EQ(TaskGraph::Get().GetPendingTaskCount(), 0);
}

TEST_F(TaskSystemTest, ComplexDependencyGraph) {
    std::vector<int> execution_order;
    std::mutex order_mutex;

    // Create a diamond dependency pattern
    //     A
    //    / \
    //   B   C
    //    \ /
    //     D

    auto taskA = TaskGraph::Get().CreateSimpleTask([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(1);
    });

    auto taskB = TaskGraph::Get().CreateTaskWithDependency([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(2);
    }, taskA);

    auto taskC = TaskGraph::Get().CreateTaskWithDependency([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(3);
    }, taskA);

    auto taskD = TaskGraph::Get().CreateTaskWithDependencies([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(4);
    }, {taskB, taskC});

    TaskGraph::Get().WaitForTask(taskD);

    // A should execute first, D should execute last
    EXPECT_EQ(execution_order[0], 1);
    EXPECT_EQ(execution_order[3], 4);

    // B and C can execute in any order after A
    EXPECT_TRUE((execution_order[1] == 2 && execution_order[2] == 3) ||
                (execution_order[1] == 3 && execution_order[2] == 2));
}

// ============================================================================
// New tests: Cancellation
// ============================================================================

TEST_F(TaskSystemTest, CancelPendingTask) {
    // Use a barrier to prevent the task from being picked up immediately
    std::atomic<bool> gate{false};

    // Create a blocking task to occupy all worker threads
    std::vector<TaskRef> blockers;
    for (int i = 0; i < 4; i++) {
        blockers.push_back(TaskGraph::Get().CreateSimpleTask([&gate]() {
            while (!gate.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }, TaskPriorities::kCritical));
    }

    // Create a task that should be cancelled before execution
    std::atomic<bool> executed{false};
    auto task = TaskGraph::Get().CreateSimpleTask([&executed]() {
        executed.store(true);
    }, TaskPriorities::kLow);

    // Cancel the task while it's pending
    task->Cancel();

    // Release the blockers
    gate.store(true);
    TaskGraph::Get().WaitForTasks(blockers);

    // Wait for the cancelled task (should return immediately)
    TaskGraph::Get().WaitForTask(task);

    // The task should NOT have executed
    EXPECT_FALSE(executed.load());
    EXPECT_EQ(task->GetState(), TaskStateType::kCancelled);
    EXPECT_TRUE(task->IsCancelled());
}

TEST_F(TaskSystemTest, CancelRunningTaskNoEffect) {
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};

    auto task = TaskGraph::Get().CreateSimpleTask([&]() {
        started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        finished.store(true);
    });

    // Wait for the task to start
    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Try to cancel while running — should have no effect
    task->Cancel();

    TaskGraph::Get().WaitForTask(task);

    // Task should complete normally
    EXPECT_TRUE(finished.load());
    EXPECT_EQ(task->GetState(), TaskStateType::kFinished);
    EXPECT_FALSE(task->IsCancelled());
}

TEST_F(TaskSystemTest, CancelBeforeFire) {
    std::atomic<bool> executed{false};

    // Create task without firing
    auto init = TaskGraph::Get().CreateTask([&executed]() {
        executed.store(true);
    });

    // Get the task ref before Done
    TaskRef task = init.Done(false); // Don't fire yet

    // Cancel before firing
    task->Cancel();

    // Now fire it
    task->Fire();

    // Give workers time to potentially pick it up
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EXPECT_FALSE(executed.load());
    EXPECT_EQ(task->GetState(), TaskStateType::kCancelled);
}

TEST_F(TaskSystemTest, CascadeCancelSuccessors) {
    std::atomic<bool> parent_executed{false};
    std::atomic<bool> child_executed{false};
    std::atomic<bool> grandchild_executed{false};

    // Block workers to keep tasks pending
    std::atomic<bool> gate{false};
    std::vector<TaskRef> blockers;
    for (int i = 0; i < 4; i++) {
        blockers.push_back(TaskGraph::Get().CreateSimpleTask([&gate]() {
            while (!gate.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }, TaskPriorities::kCritical));
    }

    // Create chain: parent → child → grandchild
    auto parent = TaskGraph::Get().CreateSimpleTask([&]() {
        parent_executed.store(true);
    }, TaskPriorities::kLow);

    auto child = TaskGraph::Get().CreateTaskWithDependency([&]() {
        child_executed.store(true);
    }, parent, TaskPriorities::kLow);

    auto grandchild = TaskGraph::Get().CreateTaskWithDependency([&]() {
        grandchild_executed.store(true);
    }, child, TaskPriorities::kLow);

    // Cancel the parent — should cascade
    parent->Cancel();

    // Release blockers
    gate.store(true);
    TaskGraph::Get().WaitForTasks(blockers);

    // Wait for all
    TaskGraph::Get().WaitForTasks({parent, child, grandchild});

    EXPECT_FALSE(parent_executed.load());
    EXPECT_FALSE(child_executed.load());
    EXPECT_FALSE(grandchild_executed.load());

    EXPECT_EQ(parent->GetState(), TaskStateType::kCancelled);
    EXPECT_EQ(child->GetState(), TaskStateType::kCancelled);
    EXPECT_EQ(grandchild->GetState(), TaskStateType::kCancelled);
}

// ============================================================================
// New tests: Continuous Priority
// ============================================================================

TEST_F(TaskSystemTest, ContinuousPriorityOrdering) {
    // Block all workers first
    std::atomic<bool> gate{false};
    std::vector<TaskRef> blockers;
    for (int i = 0; i < 4; i++) {
        blockers.push_back(TaskGraph::Get().CreateSimpleTask([&gate]() {
            while (!gate.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }, TaskPriorities::kCritical));
    }

    // Create tasks with fine-grained priorities (100, 200, 300, ..., 1000)
    const int num_tasks = 10;
    std::vector<int> execution_order;
    std::mutex order_mutex;
    std::vector<TaskRef> tasks;

    for (int i = 0; i < num_tasks; i++) {
        TaskPriority priority = (i + 1) * 100; // 100, 200, ..., 1000
        tasks.push_back(TaskGraph::Get().CreateSimpleTask([&, i, priority]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            execution_order.push_back(i);
        }, priority));
    }

    // Release blockers
    gate.store(true);
    TaskGraph::Get().WaitForTasks(blockers);

    // Wait for all tasks
    TaskGraph::Get().WaitForTasks(tasks);

    // Tasks should execute in reverse order (highest priority first)
    ASSERT_EQ(execution_order.size(), num_tasks);
    for (int i = 0; i < num_tasks; i++) {
        EXPECT_EQ(execution_order[i], num_tasks - 1 - i)
            << "Task at position " << i << " should be task " << (num_tasks - 1 - i);
    }
}

TEST_F(TaskSystemTest, UpdatePriorityChangesScheduling) {
    // Block all workers
    std::atomic<bool> gate{false};
    std::vector<TaskRef> blockers;
    for (int i = 0; i < 4; i++) {
        blockers.push_back(TaskGraph::Get().CreateSimpleTask([&gate]() {
            while (!gate.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }, TaskPriorities::kCritical));
    }

    std::vector<int> execution_order;
    std::mutex order_mutex;

    // Create two tasks: A with high priority, B with low priority
    auto task_a = TaskGraph::Get().CreateSimpleTask([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(1); // A
    }, TaskPriorities::kHigh);

    auto task_b = TaskGraph::Get().CreateSimpleTask([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(2); // B
    }, TaskPriorities::kLow);

    // Now update B to have higher priority than A
    task_b->UpdatePriority(TaskPriorities::kCritical);

    // Release blockers
    gate.store(true);
    TaskGraph::Get().WaitForTasks(blockers);

    TaskGraph::Get().WaitForTasks({task_a, task_b});

    // B should execute first (it was promoted to critical)
    ASSERT_EQ(execution_order.size(), 2);
    EXPECT_EQ(execution_order[0], 2); // B first
    EXPECT_EQ(execution_order[1], 1); // A second
}

// ============================================================================
// New tests: CancellationToken
// ============================================================================

TEST_F(TaskSystemTest, CancellationTokenCooperative) {
    auto token = Create<CancellationToken>();
    std::atomic<int> iterations{0};

    auto task = TaskGraph::Get().CreateSimpleTask([token, &iterations]() {
        while (!token->IsCancelled()) {
            iterations.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Let it run for a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Cancel via token
    token->Cancel();

    TaskGraph::Get().WaitForTask(task);

    // Should have run some iterations but stopped
    EXPECT_GT(iterations.load(), 0);
    EXPECT_LT(iterations.load(), 20); // Should not have run forever
    EXPECT_EQ(task->GetState(), TaskStateType::kFinished);
}

// ============================================================================
// New tests: TaskGroup
// ============================================================================

TEST_F(TaskSystemTest, TaskGroupCancelAll) {
    std::atomic<bool> gate{false};
    // Block workers
    std::vector<TaskRef> blockers;
    for (int i = 0; i < 4; i++) {
        blockers.push_back(TaskGraph::Get().CreateSimpleTask([&gate]() {
            while (!gate.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }, TaskPriorities::kCritical));
    }

    auto group = Create<TaskGroup>();
    auto token = group->GetToken();

    std::atomic<int> executed_count{0};

    // Add multiple tasks to the group
    for (int i = 0; i < 5; i++) {
        auto task = TaskGraph::Get().CreateSimpleTask([&, token]() {
            if (token->IsCancelled()) return;
            executed_count.fetch_add(1);
        }, TaskPriorities::kLow);
        group->Add(task);
    }

    // Cancel all before releasing blockers
    group->CancelAll();

    // Release blockers
    gate.store(true);
    TaskGraph::Get().WaitForTasks(blockers);

    // Wait for group tasks
    group->WaitAll();

    // Token should be cancelled
    EXPECT_TRUE(token->IsCancelled());

    // Tasks should be cancelled (state depends on timing)
    EXPECT_EQ(group->GetTaskCount(), (size_t)5);
}

TEST_F(TaskSystemTest, TaskGroupWaitAll) {
    auto group = Create<TaskGroup>();
    std::atomic<int> counter{0};

    for (int i = 0; i < 10; i++) {
        auto task = TaskGraph::Get().CreateSimpleTask([&counter]() {
            counter.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        group->Add(task);
    }

    group->WaitAll();

    EXPECT_EQ(counter.load(), 10);
}

// ============================================================================
// New tests: BatchEnqueue
// ============================================================================

TEST_F(TaskSystemTest, BatchEnqueueBasic) {
    std::atomic<int> counter{0};

    TaskBatch batch;
    std::vector<TaskRef> tasks;

    for (int i = 0; i < 20; i++) {
        tasks.push_back(batch.Add(
            std::move(TaskGraph::Get().CreateTask([&counter]() {
                counter.fetch_add(1);
            }).SetPriority(TaskPriorities::kNormal))
        ));
    }

    batch.Submit();

    TaskGraph::Get().WaitForTasks(tasks);

    EXPECT_EQ(counter.load(), 20);
    for (const auto& task : tasks) {
        EXPECT_EQ(task->GetState(), TaskStateType::kFinished);
    }
}

TEST_F(TaskSystemTest, BatchEnqueueWithDependencies) {
    std::atomic<int> step{0};

    // Create first task normally
    auto first = TaskGraph::Get().CreateSimpleTask([&step]() {
        step.store(1);
    });

    // Batch enqueue dependent tasks
    TaskBatch batch;
    std::vector<TaskRef> batch_tasks;

    for (int i = 0; i < 5; i++) {
        batch_tasks.push_back(batch.Add(
            std::move(TaskGraph::Get().CreateTask([&step]() {
                EXPECT_GE(step.load(), 1);
                step.fetch_add(1);
            }).DependsOn(first).SetPriority(TaskPriorities::kNormal))
        ));
    }

    batch.Submit();

    TaskGraph::Get().WaitForTasks(batch_tasks);

    EXPECT_EQ(step.load(), 6); // 1 (first) + 5 (batch)
}

// Add main function for Google Test
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
