/*
 * Created: 2025/3/29
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef DEBUG_PROF_H
#define DEBUG_PROF_H

// A very very very naive CPU time profiling tool
// only for one-time-use debug purposes

#include <chrono>
#include <map>
#include <iostream>
#include <utility>
#include "core/common.h"

MI_NAMESPACE_BEGIN


struct DebugProfSectionStatistics {
    std::string name;
    uint64_t time_ns {};
    uint32_t call_count {};
};

void DebugProfAccumulateSectionTime(const std::string & name, int64_t duration_ns);

std::map<uint32_t, DebugProfSectionStatistics> DebugProfGetSectionStatistics();

void DebugProfResetSectionTimes();
void DebugProfReset();

class ProfiledSection {
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
    std::string name_;
public:
    inline ProfiledSection(std::string name): name_(std::move(name)) {
        start_ = std::chrono::high_resolution_clock::now();
    }
    inline ~ProfiledSection() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        DebugProfAccumulateSectionTime(name_, duration);
    }
};

#ifndef NDEBUG
#define DEBUG_PROFILE_SECTION(Name) ProfiledSection zz_profiled_section_##Name(#Name)
#else
#define DEBUG_PROFILE_SECTION(Name)
#endif

MI_NAMESPACE_END
#endif //DEBUG_PROF_H
