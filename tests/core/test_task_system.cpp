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
    }, TaskPriority::kLow);

    auto high_task = TaskGraph::Get().CreateSimpleTask([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(2); // High priority
    }, TaskPriority::kHigh);

    auto normal_task = TaskGraph::Get().CreateSimpleTask([&]() {
        std::lock_guard<std::mutex> lock(order_mutex);
        execution_order.push_back(1); // Normal priority
    }, TaskPriority::kNormal);

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
        .SetPriority(TaskPriority::kHigh)
        .Done();

    TaskGraph::Get().WaitForTask(task);

    EXPECT_TRUE(executed.load());
    EXPECT_EQ(task->GetPriority(), TaskPriority::kHigh);
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

// Add main function for Google Test
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
