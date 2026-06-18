# MacroMC Profiler 设计

`macromc_prof` 模块：MacroMC 的 CPU profiler，支持区间计时（Timer）、性能计数器（Counter）、瞬时值（Gauge），多 worker 线程安全。

## 1. 为什么不用 core 的 debug_prof

`mi/core/util/debug_prof.h` 是一次性调试工具：全局 `std::map<string,...>`、仅 Debug 构建、只累加 time+count、无线程安全、无速率计数器、无查询 API。不适合 ingame 持续 profiling 和性能计数器需求。macromc 需要在真实运行/压力测试中持续观察 stage 耗时 + chunks/s 等速率指标，故新建独立 profiler。

## 2. 数据模型（三分法）

| 类型 | 语义 | API | 快照字段 | 例子 |
|---|---|---|---|---|
| **Timer** | 代码区间耗时分布 | `Scope(name)` RAII | total_ns / call_count / min / max / last（avg=total/count） | `stage.S0_registry`、`worldgen.single`、`mesh.single` |
| **Counter** | 单调递增累计，按时间算速率 | `Increment(name, delta)` | value（累计） | `worldgen_chunks_ready`、`mesh_dispatched` |
| **Gauge** | 瞬时值，可升可降 | `SetGauge(name, value)` | value（最新） | `registry_entry_count`、`streamer_envelope_size` |

三分法不合并的原因：Counter 的速率（/s）对 Gauge 无意义（entry 数不能除时间）；Timer 需要分布（min/max），前两者不需要。存储和展示都不同。

## 3. 线程模型

**mutex 保护的 per-thread accumulator**（非 thread-local 全局 registry）：

- Profiler 持有 `mutex + map<thread::id, LocalMap>`。
- worker 的 `Scope`/`Increment`/`SetGauge` 加锁写自己 thread::id 对应的 LocalMap。
- render/main 线程在帧末（S3 后，worker idle）调 `MergeFrame()`：加锁 swap 出整个 per_thread map，无锁地把每个线程的 LocalMap 折叠进全局 `global_.map`，然后清空（下一帧重新累积）。

**为什么不用无锁 thread-local**：最初用进程级 `static thread_local` registry + 裸指针，但 worker 线程退出时其 thread_local 析构，registry 里的指针悬挂 → MergeFrame 解引用崩溃（SEH 0xc0000005）。mutex 方案无悬挂指针，且 profiler 调用频率不高（worldgen/mesh 单次 ms 级），锁开销可接受。

## 4. 全局访问（worker 无持有）

Profiler 是 RefCounted 实例（app 持有 TRef），但 worker 线程不能持有 ref。`Profiler::SetGlobal(p)` / `GetGlobal()` 提供进程级裸指针槽：app init 时注册，worker 通过 `GetGlobal()` 访问。worker 生命周期被 app 包含，裸指针安全。返回 nullptr 时调用方需 null-safe（worker 代码用内联三元 + ScopeHandle 默认构造，no-op）。

## 5. 命名约定

- `stage.<name>` — tick stage 计时（`stage.S0_registry`、`stage.S1_simulation`...）
- `tick.total` — 整个 tick 计时
- `worldgen.single` — 单 chunk worldgen 计时
- `worldgen_chunks_ready` — 本 tick 收割的 ready chunk 数（counter）
- `mesh.single` — 单 chunk mesh 计时
- `mesh_dispatched` — 派发的 mesh 任务数（counter）
- `registry_entry_count` — registry entry 数（gauge）
- `streamer_envelope_size` / `streamer_pending_unloads` / `streamer_dirty_set` — streamer 状态（gauge）

## 6. 暴露

- **GetSnapshot()** — 返回所有 metric 快照（render/main 线程，MergeFrame 后）。
- **Console 命令**（`macromc_app_commands.cpp`，复用 `mi::CommandRegistry`）：
  - `mc profiler dump` — MI_LOG 打印所有 metric（Timer 显示 avg/min/max/last/count，Counter/Gauge 显示 value）。
  - `mc profiler reset` — 清空所有 metric。
  - 注意：macromc 目前无 ImGui console UI 分发命令；命令可通过编程式 `CommandRegistry::Match()` 调用。等 macromc 集成 ImGui 后即可交互。

## 7. 测试（`tests/macromc/profiler_test.cpp`，7 用例）

- Timer 计时非零 + 多次调用 avg 在 min/max 之间
- Counter 跨帧累计
- Gauge 最新值覆盖
- **多线程 merge**（4 worker × 1000 increment，MergeFrame 后全局 = 4000）
- Reset 清空
- GetGlobal/SetGlobal round-trip

## 8. 不在本轮（后续）

- ImGui profiler 面板（实时折线 + 数值表）——GetSnapshot() 已备好，等 macromc ImGui 集成
- Chrome tracing JSON 导出
- GPU timestamp（renderer 的 MI_ENABLE_TIMESTAMP 机制另算）
