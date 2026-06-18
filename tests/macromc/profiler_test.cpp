/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

// Unit tests for the MacroMC Profiler: timer / counter / gauge semantics,
// thread-local accumulation + MergeFrame reconciliation, and Reset.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "profiler/profiler.h"

using namespace macromc;

namespace {
// Find a metric snapshot by name; returns nullptr if absent.
const MetricSnapshot* FindMetric(const std::vector<MetricSnapshot>& snaps,
                                 std::string_view name) {
    for (const auto& m : snaps) if (m.name == name) return &m;
    return nullptr;
}
} // namespace

TEST(ProfilerTest, TimerRecordsNonZeroDuration) {
    auto prof = Profiler::Create();
    {
        auto s = prof->Scope("work");
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    prof->MergeFrame();
    auto snaps = prof->GetSnapshot();
    const auto* m = FindMetric(snaps, "work");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->type, MetricType::kTimer);
    EXPECT_EQ(m->call_count, 1u);
    EXPECT_GT(m->last_ns, 0u);          // measured something
    EXPECT_GE(m->total_ns, m->last_ns); // total >= last
    EXPECT_LE(m->min_ns, m->max_ns);
}

TEST(ProfilerTest, TimerAccumulatesMultipleCalls) {
    auto prof = Profiler::Create();
    for (int i = 0; i < 5; ++i) {
        auto s = prof->Scope("loop");
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    prof->MergeFrame();
    auto snaps = prof->GetSnapshot();
    const auto* m = FindMetric(snaps, "loop");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->call_count, 5u);
    // avg lies between min and max.
    double avg = static_cast<double>(m->total_ns) / m->call_count;
    EXPECT_GE(avg, static_cast<double>(m->min_ns));
    EXPECT_LE(avg, static_cast<double>(m->max_ns));
}

TEST(ProfilerTest, CounterAccumulatesAcrossFrames) {
    auto prof = Profiler::Create();
    prof->Increment("chunks", 3);
    prof->MergeFrame();
    prof->Increment("chunks", 4);
    prof->MergeFrame();
    auto snaps = prof->GetSnapshot();
    const auto* m = FindMetric(snaps, "chunks");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->type, MetricType::kCounter);
    EXPECT_EQ(m->value, 7u);  // cumulative across frames
}

TEST(ProfilerTest, GaugeLatestWins) {
    auto prof = Profiler::Create();
    prof->SetGauge("entries", 10);
    prof->MergeFrame();
    EXPECT_EQ(FindMetric(prof->GetSnapshot(), "entries")->value, 10u);
    // Within a frame, later SetGauge overwrites earlier (last-set per frame).
    prof->SetGauge("entries", 5);
    prof->SetGauge("entries", 20);
    prof->MergeFrame();
    // The merge takes the local accumulator's value (last set in the frame).
    // Since we don't have concurrent threads here, the single-threaded local
    // map holds 20 as the final value.
    EXPECT_EQ(FindMetric(prof->GetSnapshot(), "entries")->value, 20u);
}

TEST(ProfilerTest, MergeFrameReconcilesWorkerThreads) {
    // Two worker threads each increment a counter N times; after MergeFrame the
    // global total must equal the sum. This exercises thread-local accumulation.
    auto prof = Profiler::Create();
    constexpr int kPerThread = 1000;
    constexpr int kThreads = 4;
    std::vector<std::thread> workers;
    std::atomic<int> started{0};
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&prof, &started, kPerThread]() {
            // Touch the profiler to register this thread's local accumulator.
            prof->Increment("warmup", 0);
            started.fetch_add(1);
            while (started.load() < kThreads) { /* spin until all ready */ }
            for (int i = 0; i < kPerThread; ++i) prof->Increment("ticks", 1);
        });
    }
    for (auto& w : workers) w.join();
    // Workers are now idle — safe to merge.
    prof->MergeFrame();
    auto snaps = prof->GetSnapshot();
    const auto* m = FindMetric(snaps, "ticks");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->value, static_cast<uint64_t>(kPerThread * kThreads));
}

TEST(ProfilerTest, ResetClearsAllMetrics) {
    auto prof = Profiler::Create();
    prof->Increment("a", 1);
    prof->SetGauge("b", 2);
    { auto s = prof->Scope("c"); }
    prof->MergeFrame();
    ASSERT_FALSE(prof->GetSnapshot().empty());
    prof->Reset();
    EXPECT_TRUE(prof->GetSnapshot().empty());
}

TEST(ProfilerTest, GlobalAccessorRoundTrip) {
    // SetGlobal installs a raw pointer reachable from any thread; used by
    // worker code that can't hold a ref. Verify it's null by default, settable,
    // and clearable.
    EXPECT_EQ(Profiler::GetGlobal(), nullptr);
    auto prof = Profiler::Create();
    Profiler::SetGlobal(prof.Raw());
    EXPECT_EQ(Profiler::GetGlobal(), prof.Raw());
    Profiler::SetGlobal(nullptr);
    EXPECT_EQ(Profiler::GetGlobal(), nullptr);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
