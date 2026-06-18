/*
 * Created: 2026/06/17
 * Author:  hineven
 * See LICENSE for licensing.
 */

// Console command registration for MacroMC. Mirrors the viewer_commands.cpp
// pattern: each command is registered into the global mi::CommandRegistry via
// MakeAndRegister, and its callback prints results through MI_LOG (there is no
// separate "console output API"; the console UI / std::out consumes log lines).
//
// Note: MacroMC does not yet have an interactive ImGui console UI to dispatch
// typed input through CommandRegistry::Match. Until it does, these commands are
// reachable programmatically (e.g. a test or a scripted harness calls Match()).

#include "macromc_app.h"

#include <string>
#include <vector>

#include "core/infra.h"
#include "core/util/command_line.h"
#include "profiler/profiler.h"

MACROMC_NAMESPACE_BEGIN

namespace {

// Format a single metric snapshot as one log line. Timers show a distribution
// (avg/min/max/last + count); counters/gauges show their value.
std::string FormatMetric(const MetricSnapshot& m) {
    auto us = [](uint64_t ns) { return ns / 1000.0; };  // ns -> microseconds
    if (m.type == MetricType::kTimer) {
        double avg = m.call_count ? us(m.total_ns / m.call_count) : 0.0;
        return std::format("  [T] {:<28} avg={:.1f}us min={:.1f}us max={:.1f}us "
                           "last={:.1f}us n={}",
                           m.name, avg, us(m.min_ns), us(m.max_ns), us(m.last_ns),
                           m.call_count);
    }
    const char* tag = (m.type == MetricType::kCounter) ? "[C]" : "[G]";
    return std::format("  {} {:<28} value={}", tag, m.name, m.value);
}

} // namespace

void RegisterMacroMCCommands(MacroMCApp& app) {
    // mc profiler dump — print all collected metrics.
    mi::CommandRegistry::Get().MakeAndRegister(
        "mc.profiler.dump",
        {mi::CommandTokenSpec::KeywordSet({"mc"}),
         mi::CommandTokenSpec::KeywordSet({"profiler"}),
         mi::CommandTokenSpec::KeywordSet({"dump"})},
        [prof = app.GetProfiler()](const mi::CommandMatchResult&) {
            if (!prof) {
                MI_LOG(mi::MIInfraLogType::kWarning, "mc.profiler.dump: no active profiler");
                return;
            }
            auto snaps = prof->GetSnapshot();
            MI_LOG(mi::MIInfraLogType::kInfo,
                   "mc.profiler.dump: {} metrics", snaps.size());
            for (const auto& m : snaps) {
                MI_LOG(mi::MIInfraLogType::kInfo, "{}", FormatMetric(m));
            }
        });

    // mc profiler reset — clear all metrics.
    mi::CommandRegistry::Get().MakeAndRegister(
        "mc.profiler.reset",
        {mi::CommandTokenSpec::KeywordSet({"mc"}),
         mi::CommandTokenSpec::KeywordSet({"profiler"}),
         mi::CommandTokenSpec::KeywordSet({"reset"})},
        [prof = app.GetProfiler()](const mi::CommandMatchResult&) {
            if (prof) {
                prof->Reset();
                MI_LOG(mi::MIInfraLogType::kInfo, "mc.profiler.reset: metrics cleared");
            }
        });
}

MACROMC_NAMESPACE_END
