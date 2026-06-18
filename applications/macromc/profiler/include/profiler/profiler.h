/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_PROFILER_PROFILER_H
#define MACROMC_PROFILER_PROFILER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "profiler/common.h"
#include "core/refcounted.h"

MACROMC_PROFILER_NAMESPACE_BEGIN

// =============================================================================
// Profiler — CPU profiling for MacroMC: time-interval timers + performance
// counters + instantaneous gauges, with thread-local accumulation and a
// per-frame merge so multi-threaded workers (worldgen, mesh) record lock-free
// and the render thread reconciles once per frame.
//
// Data model (three metric kinds):
//   - Timer  : a code region's duration distribution. Updated via RAII Scope().
//              Snapshot carries total_ns / call_count / min_ns / max_ns / last_ns
//              (avg = total_ns / call_count).
//   - Counter: a monotonically increasing total (e.g. chunks generated). Use
//              Increment(). Rate (per second) is derived from value delta over
//              wall-clock time by the caller / dump formatter.
//   - Gauge  : an instantaneous value that can go up or down (e.g. registry
//              entry count). Use SetGauge(); the latest set wins.
//
// Threading model:
//   - Each worker thread accumulates into a thread-local map (lock-free writes).
//   - The render/main thread calls MergeFrame() once per frame at the S3-done
//     sync point. At that moment all workers are idle (worker activity is
//     confined to S0..S2 per TICK_PACING; S3 is single-threaded), so the merge
//     can safely read every thread's local map without tearing.
//   - GetSnapshot()/Reset() are only valid from the render/main thread after a
//     merge (they read the global aggregated map).
//
// Global access:
//   - The profiler is owned by the app as a TRef, but worker threads can't hold
//     a ref (they'd extend its lifetime / need to pass it in). SetGlobal()
//     registers a raw pointer the app installs at init; GetGlobal() lets any
//     worker reach it. Worker lifetimes are bounded by the app's, so the raw
//     pointer is safe. GetGlobal() returns nullptr if no profiler is active
//     (e.g. in a unit test that only exercises a subset); callers should
//     null-check or use the MI_PROF_* macros below which no-op on null.
// =============================================================================

enum class MetricType : uint8_t {
    kTimer,
    kCounter,
    kGauge,
};

// A point-in-time view of one metric, produced by GetSnapshot(). Timer fields
// (total_ns/call_count/min/max/last) are meaningful for kTimer; `value` is
// meaningful for kCounter (cumulative) and kGauge (latest).
struct MetricSnapshot {
    std::string name;
    MetricType type = MetricType::kCounter;
    // Timer distribution (kTimer only).
    uint64_t total_ns = 0;
    uint64_t call_count = 0;
    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;
    uint64_t last_ns = 0;
    // Counter (cumulative) / Gauge (latest set).
    uint64_t value = 0;
};

class Profiler : public mi::RefCounted<> {
public:
    static mi::TRef<Profiler> Create();

    // --- RAII timer scope. Cheap to construct: records a begin timestamp and
    // the metric name (const char* — must outlive the scope, so prefer string
    // literals). On destruction, records the elapsed ns into this thread's
    // local accumulator. Nesting is fine (each scope is independent by name).
    class ScopeHandle {
    public:
        ScopeHandle() = default;
        ScopeHandle(Profiler* p, const char* name, uint64_t begin_ns)
            : profiler_(p), name_(name), begin_ns_(begin_ns) {}
        ScopeHandle(ScopeHandle&& other) noexcept
            : profiler_(other.profiler_), name_(other.name_), begin_ns_(other.begin_ns_) {
            other.profiler_ = nullptr;
        }
        ScopeHandle& operator=(ScopeHandle&& other) noexcept {
            if (this != &other) { Finish(); profiler_ = other.profiler_; name_ = other.name_;
                                  begin_ns_ = other.begin_ns_; other.profiler_ = nullptr; }
            return *this;
        }
        ScopeHandle(const ScopeHandle&) = delete;
        ScopeHandle& operator=(const ScopeHandle&) = delete;
        ~ScopeHandle() { Finish(); }
    private:
        void Finish();  // records into the thread-local accumulator if active
        Profiler* profiler_ = nullptr;
        const char* name_ = nullptr;
        uint64_t begin_ns_ = 0;
    };
    ScopeHandle Scope(const char* name);

    // --- Counter (monotonic). Adds delta to the named metric.
    void Increment(const char* name, uint64_t delta = 1);
    // --- Gauge (instantaneous). Latest SetGauge wins for the frame.
    void SetGauge(const char* name, uint64_t value);

    // --- Frame lifecycle (render/main thread only). Merges every worker's
    // thread-local accumulator into the global aggregated map and clears the
    // thread-locals so the next frame starts fresh. Must be called once per
    // frame at a point where all workers are idle.
    void MergeFrame();

    // --- Query (render/main thread only, after MergeFrame). Returns snapshots
    // for every metric seen since the last Reset(), in insertion order.
    std::vector<MetricSnapshot> GetSnapshot() const;

    // --- Clear all metrics (render/main thread only).
    void Reset();

    // --- Global access for worker threads. The app installs the active
    // profiler at init; workers read it via GetGlobal(). Returns nullptr if
    // none is active (callers / MI_PROF_* macros handle that).
    static void SetGlobal(Profiler* p);
    static Profiler* GetGlobal();

private:
    Profiler();
    ~Profiler();  // defined in .cpp (ThreadData/GlobalState incomplete here)

    // Per-thread accumulators (mutex-guarded). Defined in the .cpp.
    struct ThreadData;
    std::unique_ptr<ThreadData> thread_data_;

    // Aggregated stats after MergeFrame. Held behind a unique_ptr to a forward-
    // declared struct so the header needn't include <unordered_map>. Only the
    // render/main thread touches this.
    struct GlobalState;
    std::unique_ptr<GlobalState> global_;
};

// -----------------------------------------------------------------------------
// Convenience macros — null-safe so call sites in hot paths (worldgen, mesh)
// don't need to branch. They no-op entirely when no profiler is active.
// `name` must be a string literal (or otherwise outlive the scope).
#define MI_PROF_SCOPE(name) \
    ::macromc::Profiler::ScopeHandle _mi_prof_scope_##name( \
        ::macromc::Profiler::GetGlobal() ? ::macromc::Profiler::GetGlobal()->Scope(#name) \
                                          : ::macromc::Profiler::ScopeHandle())
#define MI_PROF_INCREMENT(name, delta) \
    do { if (auto* _mi_prof = ::macromc::Profiler::GetGlobal()) _mi_prof->Increment(#name, delta); } while (0)
#define MI_PROF_GAUGE(name, value) \
    do { if (auto* _mi_prof = ::macromc::Profiler::GetGlobal()) _mi_prof->SetGauge(#name, value); } while (0)

MACROMC_PROFILER_NAMESPACE_END

#endif // MACROMC_PROFILER_PROFILER_H
