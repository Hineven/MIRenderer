/*
 * Created: 2026/1/4
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <map>
#include <xxhash.h>
#include <string>
#include <mutex>
#include "core/util/debug_prof.h"
#include "core/common.h"
MI_NAMESPACE_BEGIN

static std::map<uint32_t, DebugProfSectionStatistics> section_time_map;
static std::mutex section_time_map_mutex;

void DebugProfAccumulateSectionTime(const std::string & name, int64_t duration_ns) {
    std::lock_guard<std::mutex> lock(section_time_map_mutex);
    uint32_t hash = XXH32(name.data(), name.length(), 0);
    auto & section_time = section_time_map[hash];
    section_time.time_ns += uint64_t(duration_ns);
    section_time.call_count += 1;
    if (section_time.name.empty() && !name.empty()) {
        section_time.name = name;
    }
}

std::map<uint32_t, DebugProfSectionStatistics> DebugProfGetSectionStatistics() {
    std::lock_guard<std::mutex> lock(section_time_map_mutex);
    return section_time_map;
}

void DebugProfResetSectionTimes() {
    std::lock_guard<std::mutex> lock(section_time_map_mutex);
    for (auto & e : section_time_map) {
        e.second.time_ns = 0;
        e.second.call_count = 0;
    }
}

void DebugProfReset() {
    std::lock_guard<std::mutex> lock(section_time_map_mutex);
    section_time_map.clear();
}

MI_NAMESPACE_END