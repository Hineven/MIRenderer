/*
 * Created: 2025/3/29
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef DEBUG_PROF_H
#define DEBUG_PROF_H

// A very very very naive profiling tool
// only for one-time-use debug purposes

#include <chrono>
#include <iostream>

class ProfiledSection {
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
    std::string name_;
public:
    inline ProfiledSection(std::string name): name_(std::move(name)) {
        start_ = std::chrono::high_resolution_clock::now();
    }
    inline ~ProfiledSection() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        std::cout << "ProfiledSection: " << name_ << " took " << duration << " us." << std::endl;
    }
};

#define PROFILE_SECTION(Name) ProfiledSection zz_profiled_section_##Name(#Name)

#endif //DEBUG_PROF_H
