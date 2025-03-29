#include <chrono>
#include <iostream>

class ProfiledSection {
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
    std::string name_;
public:
    FORCEINLINE ProfiledSection(std::string name): name_(std::move(name)) {
        start_ = std::chrono::high_resolution_clock::now();
    }
    FORCEINLINE ~ProfiledSection() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        std::cout << "ProfiledSection: " << name_ << " took " << duration << " us." << std::endl;
    }
};

#define PROFILE_SECTION(Name) ProfiledSection zz_profiled_section_##Name(#Name)