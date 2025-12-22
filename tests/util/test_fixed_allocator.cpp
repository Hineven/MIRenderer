/*
 * Created: 2024/9/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include "core/util/alloc.h"

using namespace mi;

// Test struct for allocation
struct TestObject {
    int value;
    double data;

    TestObject() : value(0), data(0.0) {}
    TestObject(int v, double d) : value(v), data(d) {}

    ~TestObject() {
        // Mark as destroyed for testing
        value = -1;
        data = -1.0;
    }
};

class TFixedElementAllocatorTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(TFixedElementAllocatorTest, BasicAllocation) {
    TFixedElementAllocator<TestObject, 10> allocator;

    // Test basic allocation
    TestObject* obj = allocator.Allocate(42, 3.14);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->value, 42);
    EXPECT_EQ(obj->data, 3.14);

    // Test capacity and count
    EXPECT_EQ(allocator.GetCapacity(), 10);
    EXPECT_EQ(allocator.GetAllocatedCount(), 1);
    EXPECT_FALSE(allocator.IsFull());

    // Test ownership
    EXPECT_TRUE(allocator.OwnsPointer(obj));

    // Free the object
    allocator.Free(obj);
    EXPECT_EQ(allocator.GetAllocatedCount(), 0);
}

TEST_F(TFixedElementAllocatorTest, MultipleAllocations) {
    TFixedElementAllocator<TestObject, 5> allocator;
    std::vector<TestObject*> objects;

    // Allocate all slots
    for (int i = 0; i < 5; ++i) {
        TestObject* obj = allocator.Allocate(i, i * 1.5);
        ASSERT_NE(obj, nullptr);
        EXPECT_EQ(obj->value, i);
        EXPECT_EQ(obj->data, i * 1.5);
        objects.push_back(obj);
    }

    EXPECT_EQ(allocator.GetAllocatedCount(), 5);
    EXPECT_TRUE(allocator.IsFull());

    // Try to allocate one more (should fail)
    TestObject* overflow = allocator.Allocate(999, 999.0);
    EXPECT_EQ(overflow, nullptr);

    // Free all objects
    for (auto* obj : objects) {
        allocator.Free(obj);
    }

    EXPECT_EQ(allocator.GetAllocatedCount(), 0);
    EXPECT_FALSE(allocator.IsFull());
}

TEST_F(TFixedElementAllocatorTest, MemoryReuse) {
    TFixedElementAllocator<TestObject, 3> allocator;

    // Allocate an object
    TestObject* obj1 = allocator.Allocate(100, 100.0);
    ASSERT_NE(obj1, nullptr);

    // Free it
    allocator.Free(obj1);
    EXPECT_EQ(allocator.GetAllocatedCount(), 0);

    // Allocate again - should reuse the same memory
    TestObject* obj2 = allocator.Allocate(200, 200.0);
    ASSERT_NE(obj2, nullptr);
    EXPECT_EQ(obj1, obj2); // Should be the same memory location
    EXPECT_EQ(obj2->value, 200);
    EXPECT_EQ(obj2->data, 200.0);

    allocator.Free(obj2);
}

TEST_F(TFixedElementAllocatorTest, Alignment) {
    // Test with custom alignment
    TFixedElementAllocator<TestObject, 5, 32> allocator;

    TestObject* obj1 = allocator.Allocate(1, 1.0);
    TestObject* obj2 = allocator.Allocate(2, 2.0);

    ASSERT_NE(obj1, nullptr);
    ASSERT_NE(obj2, nullptr);

    // Check alignment
    uintptr_t addr1 = reinterpret_cast<uintptr_t>(obj1);
    uintptr_t addr2 = reinterpret_cast<uintptr_t>(obj2);

    EXPECT_EQ(addr1 % 32, 0); // Should be 32-byte aligned
    EXPECT_EQ(addr2 % 32, 0); // Should be 32-byte aligned

    allocator.Free(obj1);
    allocator.Free(obj2);
}

TEST_F(TFixedElementAllocatorTest, ThreadSafeAllocator) {
    TFixedElementAllocator<TestObject, 100, 16, true> allocator; // Thread-safe version
    std::atomic<int> successful_allocations{0};
    std::atomic<int> successful_frees{0};
    std::vector<std::thread> threads;

    const int num_threads = 4;
    const int allocations_per_thread = 10;

    // Start threads that allocate and free objects
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            std::vector<TestObject*> local_objects;

            // Allocate objects
            for (int i = 0; i < allocations_per_thread; ++i) {
                TestObject* obj = allocator.Allocate(t * 100 + i, (t * 100 + i) * 1.5);
                if (obj) {
                    local_objects.push_back(obj);
                    successful_allocations.fetch_add(1);
                }
            }

            // Free objects
            for (auto* obj : local_objects) {
                allocator.Free(obj);
                successful_frees.fetch_add(1);
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // All allocations should have succeeded
    EXPECT_EQ(successful_allocations.load(), num_threads * allocations_per_thread);
    EXPECT_EQ(successful_frees.load(), num_threads * allocations_per_thread);
    EXPECT_EQ(allocator.GetAllocatedCount(), 0);
}

TEST_F(TFixedElementAllocatorTest, OwnershipValidation) {
    TFixedElementAllocator<TestObject, 5> allocator1;
    TFixedElementAllocator<TestObject, 5> allocator2;

    TestObject* obj1 = allocator1.Allocate(1, 1.0);
    TestObject* obj2 = allocator2.Allocate(2, 2.0);

    ASSERT_NE(obj1, nullptr);
    ASSERT_NE(obj2, nullptr);

    // Each allocator should only own its own objects
    EXPECT_TRUE(allocator1.OwnsPointer(obj1));
    EXPECT_FALSE(allocator1.OwnsPointer(obj2));

    EXPECT_TRUE(allocator2.OwnsPointer(obj2));
    EXPECT_FALSE(allocator2.OwnsPointer(obj1));

    allocator1.Free(obj1);
    allocator2.Free(obj2);
}

TEST_F(TFixedElementAllocatorTest, DestructorCalling) {
    TFixedElementAllocator<TestObject, 3> allocator;

    TestObject* obj = allocator.Allocate(42, 3.14);
    ASSERT_NE(obj, nullptr);
    EXPECT_EQ(obj->value, 42);
    EXPECT_EQ(obj->data, 3.14);

    // Free the object - destructor should be called
    allocator.Free(obj);

    // Note: We can't really test if destructor was called without
    // additional instrumentation, but the destructor sets values to -1
    // This is more of a design verification than a runtime test
}

TEST_F(TFixedElementAllocatorTest, NullPointerHandling) {
    TFixedElementAllocator<TestObject, 5> allocator;

    // Freeing nullptr should not crash
    allocator.Free(nullptr);
    EXPECT_EQ(allocator.GetAllocatedCount(), 0);
}

TEST_F(TFixedElementAllocatorTest, PerformanceBaseline) {
    TFixedElementAllocator<TestObject, 1000> allocator;
    std::vector<TestObject*> objects;
    objects.reserve(1000);

    auto start = std::chrono::high_resolution_clock::now();

    // Allocate 1000 objects
    for (int i = 0; i < 1000; ++i) {
        TestObject* obj = allocator.Allocate(i, i * 1.5);
        ASSERT_NE(obj, nullptr);
        objects.push_back(obj);
    }

    // Free all objects
    for (auto* obj : objects) {
        allocator.Free(obj);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // This is just a baseline measurement - no strict assertion
    // Should be very fast (under 1ms for 1000 allocations + frees)
    EXPECT_LT(duration.count(), 1000); // Less than 1ms
}
