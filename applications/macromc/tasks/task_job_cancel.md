# Task: TaskGraph — Task 取消支持

**Status**: Planned
**Priority**: P0 (GigaVoxel Streaming 前置)
**Depends on**: 无
**Estimated Effort**: Small

---

## 1. 背景

GigaVoxel streaming pipeline 会产生大量异步 task（decompress、mesh bake、BLAS build）。当 chunk 离开视距或 LOD 需求变化时，已排队的 task 可能不再需要执行。当前 TaskGraph 没有取消机制，这些 task 会白跑完，浪费 CPU 时间。

## 2. 需求

- Task 可以被标记为 cancelled
- Worker thread 在执行 task 前检查 cancelled 状态，跳过已取消的 task
- 取消操作是线程安全的
- 已取消的 task 的 future 应该有明确的状态（而非永远 pending）

## 3. 设计方案

### 3.1 新增状态

在 `TaskStateType` 中新增 `kCancelled`：

```
kUninitialized → kReady → kPending → kRunning → kFinished
                                      ↓
                                  kCancelled（可从 kReady/kPending 转换）
```

### 3.2 新增 API

- `Task::Cancel()` — 标记 task 为 cancelled（线程安全）
- `Task::IsCancelled()` — 查询是否已取消
- Worker thread 在 `WaitAndGetNextTask()` 返回后检查 cancelled，如果已取消则跳过执行并标记 kCancelled
- 已取消 task 的 promise 设置 exception（`std::future_error` with `future_errc::broken_promise`）或通过新增 `TaskResult` 枚举表达

### 3.3 注意点

- Cancel 不中断正在 Running 的 task（不支持 cooperative cancellation within task body，初期不需要）
- Cancel 一个已 Finished 的 task 无效
- Cancel 后，依赖该 task 的 successor 应该怎么处理？
  - 建议：successor 也自动取消（级联取消），除非 successor 有其他未取消的 precedent
- Priority queue 中的 cancelled task 在 dequeue 时跳过（惰性清理）

## 4. 验收标准

1. 单元测试：Cancel pending task → 不执行
2. 单元测试：Cancel running task → 无效（仍执行完毕）
3. 单元测试：Cancel task with successors → successors 也取消
4. 性能：Cancel 操作不引入显著锁开销

## 5. 影响范围

- `mi/core/include/core/task.h` — Task 类新增方法
- `mi/core/task.cpp` — Worker thread 逻辑修改
- `tests/core/test_task_system.cpp` — 新增测试
