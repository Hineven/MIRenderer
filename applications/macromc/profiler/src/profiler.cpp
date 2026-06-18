/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "profiler/profiler.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

MACROMC_PROFILER_NAMESPACE_BEGIN

namespace {

// One per metric per thread.
//
//   kTimer: total_ns/call_count/min/max/last accumulate durations. value unused.
//   kCounter: value accumulates (Increment adds to it). Timer fields unused.
//   kGauge: value holds the latest SetGauge. Timer fields unused.
struct LocalAccum {
    MetricType type = MetricType::kCounter;
    uint64_t total_ns = 0;
    uint64_t call_count = 0;
    uint64_t min_ns = UINT64_MAX;
    uint64_t max_ns = 0;
    uint64_t last_ns = 0;
    uint64_t value = 0;
};

using LocalMap = std::unordered_map<std::string, LocalAccum>;

uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

} // namespace

// Aggregated stats, written only by MergeFrame (render/main thread). No sync.
using GlobalMap = std::unordered_map<std::string, LocalAccum>;

struct Profiler::GlobalState {
    GlobalMap map;
};

// --- Per-thread accumulators owned by the Profiler ---
// We keep a mutex-guarded map keyed by thread::id. Workers take the lock to
// write their local entries; MergeFrame takes the lock to read + clear. This
// avoids the dangling-pointer hazard of a process-wide thread_local registry
// (a thread's thread_local storage is destroyed on thread exit, but the
// registry would still hold a pointer to it). Worker writes are not hot enough
// (worldgen/mesh are ms-per-call) for the lock to matter.
struct Profiler::ThreadData {
    std::mutex mu;
    // thread::id -> that thread's accumulator. Entries are removed in
    // MergeFrame after being folded into the global map, so the map only ever
    // holds in-flight (current-frame, not-yet-merged) data.
    std::unordered_map<std::thread::id, LocalMap> per_thread;
};

mi::TRef<Profiler> Profiler::Create() {
    return mi::TRef<Profiler>(new Profiler());
}

Profiler::Profiler()
    : thread_data_(std::make_unique<ThreadData>()),
      global_(std::make_unique<GlobalState>()) {}
Profiler::~Profiler() = default;  // ThreadData/GlobalState complete in this TU

// Helper: apply one local accumulator entry into the global aggregated map.
static void MergeLocalIntoGlobal(GlobalMap& g, const std::string& name,
                                 const LocalAccum& src) {
    auto& dst = g[name];
    dst.type = src.type;
    dst.total_ns += src.total_ns;
    dst.call_count += src.call_count;
    if (src.min_ns < dst.min_ns) dst.min_ns = src.min_ns;
    if (src.max_ns > dst.max_ns) dst.max_ns = src.max_ns;
    dst.last_ns = src.last_ns;
    if (src.type == MetricType::kCounter) {
        dst.value += src.value;     // counters accumulate across frames
    } else if (src.type == MetricType::kGauge) {
        dst.value = src.value;      // gauges: latest wins
    }
}

Profiler::ScopeHandle Profiler::Scope(const char* name) {
    return ScopeHandle(this, name, NowNs());
}

void Profiler::ScopeHandle::Finish() {
    if (!profiler_) return;
    uint64_t elapsed = [] {
        uint64_t end = NowNs();
        return end;  // begin_ns_ subtracted by caller
    }();
    elapsed = (elapsed >= begin_ns_) ? (elapsed - begin_ns_) : 0;
    std::lock_guard lock(profiler_->thread_data_->mu);
    auto& local = profiler_->thread_data_->per_thread[std::this_thread::get_id()];
    auto& acc = local[name_];
    acc.type = MetricType::kTimer;
    acc.total_ns += elapsed;
    acc.call_count += 1;
    if (elapsed < acc.min_ns) acc.min_ns = elapsed;
    if (elapsed > acc.max_ns) acc.max_ns = elapsed;
    acc.last_ns = elapsed;
    profiler_ = nullptr;  // idempotent
}

void Profiler::Increment(const char* name, uint64_t delta) {
    std::lock_guard lock(thread_data_->mu);
    auto& local = thread_data_->per_thread[std::this_thread::get_id()];
    auto& acc = local[name];
    if (acc.type != MetricType::kCounter) { acc.type = MetricType::kCounter; acc.value = 0; }
    acc.value += delta;
}

void Profiler::SetGauge(const char* name, uint64_t value) {
    std::lock_guard lock(thread_data_->mu);
    auto& local = thread_data_->per_thread[std::this_thread::get_id()];
    auto& acc = local[name];
    acc.type = MetricType::kGauge;
    acc.value = value;
}

void Profiler::MergeFrame() {
    // Render/main thread only. Fold every thread's local accumulator into the
    // global map, then clear the locals so next frame starts fresh.
    std::unordered_map<std::thread::id, LocalMap> snapshot;
    {
        std::lock_guard lock(thread_data_->mu);
        snapshot.swap(thread_data_->per_thread);  // steal + clear under lock
    }
    for (auto& [tid, local_map] : snapshot) {
        for (auto& [name, src] : local_map) {
            MergeLocalIntoGlobal(global_->map, name, src);
        }
    }
}

std::vector<MetricSnapshot> Profiler::GetSnapshot() const {
    std::vector<MetricSnapshot> out;
    out.reserve(global_->map.size());
    for (auto& [name, acc] : global_->map) {
        MetricSnapshot s;
        s.name = name;
        s.type = acc.type;
        s.total_ns = acc.total_ns;
        s.call_count = acc.call_count;
        s.min_ns = acc.min_ns;
        s.max_ns = acc.max_ns;
        s.last_ns = acc.last_ns;
        s.value = acc.value;
        out.push_back(std::move(s));
    }
    return out;
}

void Profiler::Reset() {
    global_->map.clear();
    std::lock_guard lock(thread_data_->mu);
    thread_data_->per_thread.clear();
}

// --- Global single-slot pointer for worker access ---
namespace {
std::atomic<Profiler*> g_global_profiler{nullptr};
}
void Profiler::SetGlobal(Profiler* p) { g_global_profiler.store(p, std::memory_order_release); }
Profiler* Profiler::GetGlobal() { return g_global_profiler.load(std::memory_order_acquire); }

MACROMC_PROFILER_NAMESPACE_END
