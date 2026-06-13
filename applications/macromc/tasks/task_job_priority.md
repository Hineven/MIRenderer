# Task: TaskGraph — 动态优先级 + 连续优先级值

**Status**: Planned
**Priority**: P0 (GigaVoxel Streaming 前置)
**Depends on**: 无
**Estimated Effort**: Small~Medium

---

## 1. 背景

GigaVoxel streaming 需要根据 chunk 到相机的距离动态调整 task 优先级。当相机移动时，之前低优先级的 chunk 可能变成高优先级（反之亦然）。当前 TaskGraph 有两个限制：

1. **优先级不可修改**：`TaskPriority` 在创建时设定，之后无法更改
2. **优先级粒度不足**：只有 4 级（Low/Normal/High/Critical），无法支持按距离排序的精细调度

## 2. 需求

### 2.1 连续优先级值

- `TaskPriority` 从 enum 改为 `uint32_t`，值越大优先级越高
- 保留 4 个 named constant 作为通用级别：
  ```
  kPriorityLow      = 0
  kPriorityNormal   = UINT32_MAX / 4
  kPriorityHigh     = UINT32_MAX / 2
  kPriorityCritical = UINT32_MAX
  ```
- GigaVoxel streaming 可以将距离映射为连续优先级值（如 `priority = max_distance - chunk_distance`）

### 2.2 动态优先级更新

- 提供 `Task::UpdatePriority(uint32_t new_priority)` 方法
- 线程安全（可以在任意线程调用）
- 更新后立即影响调度顺序（下一次 dequeue 时按新优先级排序）

## 3. 设计方案

### 3.1 类型变更

```cpp
// Before:
enum class TaskPriority : uint32_t {
    kLow = 0, kNormal = 1, kHigh = 2, kCritical = 3, kMax
};

// After:
using TaskPriority = uint32_t;
namespace TaskPriorities {
    constexpr TaskPriority kLow      = 0;
    constexpr TaskPriority kNormal   = UINT32_MAX / 4;
    constexpr TaskPriority kHigh     = UINT32_MAX / 2;
    constexpr TaskPriority kCritical = UINT32_MAX;
}
```

### 3.2 Priority Queue 更新策略

`std::priority_queue` 不支持原地更新元素优先级。两种方案：

**方案 A：惰性更新（推荐）**
- `UpdatePriority()` 只更新 task 内部的 `priority_` 字段
- Priority queue 的 comparator 读取实时 `priority_` 值
- 问题：`std::priority_queue` 的 heap 结构在元素值变化后不再有序
- 解决：不使用 `std::priority_queue`，改用 `std::set` 或自定义 heap + reheap 操作

**方案 B：Remove + Re-insert**
- `UpdatePriority()` 从 queue 中移除 task，更新优先级，重新插入
- 需要 O(n) 查找 + O(log n) 插入
- 简单但查找开销大

**推荐方案 A**：用 `std::set<Task*, TaskComparator>` 替代 `std::priority_queue`：
- `set` 支持 erase + re-insert（O(log n)）
- `UpdatePriority()` 时：lock → erase → update → insert → unlock
- `WaitAndGetNextTask()` 时：取 `rbegin()`（最大优先级）

### 3.3 TaskComparator 更新

```cpp
struct TaskComparator {
    bool operator()(const Task* lhs, const Task* rhs) const {
        if (lhs->GetPriority() != rhs->GetPriority())
            return lhs->GetPriority() < rhs->GetPriority(); // higher value = higher priority
        return lhs->GetCreationTime() > rhs->GetCreationTime(); // FIFO for same priority
    }
};
```

## 4. 验收标准

1. 单元测试：连续优先级排序正确（100 个不同优先级的 task 按序执行）
2. 单元测试：UpdatePriority 后调度顺序立即改变
3. 单元测试：现有 4 级 named constants 行为不变（向后兼容）
4. 性能：UpdatePriority 操作 < 1μs（set erase+insert）
5. 性能：高 task count 下 dequeue 性能不退化

## 5. 影响范围

- `mi/core/include/core/task.h` — TaskPriority 类型变更，Task 新增 UpdatePriority
- `mi/core/task.cpp` — priority queue → set 替换，Worker 调度逻辑
- `mi/core/include/core/constants.h` — 如有 kMaxTaskGraphTaskCount 等常量
- `tests/core/test_task_system.cpp` — 新增测试，确保现有测试仍通过

## 6. 风险

- `std::set` 的常数因子比 `std::priority_queue` 大，高频 dequeue 场景需 profiling 验证
- 如果性能不满足，可考虑自定义 binary heap + index map（支持 O(log n) 的 decrease-key 操作）
