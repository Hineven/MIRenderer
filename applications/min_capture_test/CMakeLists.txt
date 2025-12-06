cmake_minimum_required(VERSION 3.20)
project(min_capture_test)

# Require Vulkan and GLFW
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

add_executable(min_capture_test
    src/main.cpp
)

# Include dirs
target_include_directories(min_capture_test PRIVATE ${Vulkan_INCLUDE_DIRS})

# Link Vulkan and GLFW; on Windows also link the loader library explicitly.
if (WIN32)
    target_link_libraries(min_capture_test PRIVATE Vulkan::Vulkan glfw)
else()
    target_link_libraries(min_capture_test PRIVATE Vulkan::Vulkan glfw)
endif()

# Dispatcher macros (redundant with source file but safe)
target_compile_definitions(min_capture_test PRIVATE VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)

# Use C++20
target_compile_features(min_capture_test PRIVATE cxx_std_20)

# Enable MSVC warnings sensibly
if (MSVC)
    target_compile_options(min_capture_test PRIVATE /W4)
else()
    target_compile_options(min_capture_test PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
endif()
