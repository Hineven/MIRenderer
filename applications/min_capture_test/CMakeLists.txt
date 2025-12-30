cmake_minimum_required(VERSION 3.20)
project(min_capture_test)

# Require Vulkan and GLFW
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

add_executable(min_capture_test
    src/main.cpp
)

add_executable(min_capture_test2
    src/main2.cpp
)

# Include dirs
target_include_directories(min_capture_test PRIVATE ${Vulkan_INCLUDE_DIRS})
target_include_directories(min_capture_test2 PRIVATE ${Vulkan_INCLUDE_DIRS})

# VMA-Hpp (repo submodule or vcpkg, as configured by top-level)
# Top-level CMake defines VMA_HPP_TARGET_NAME;
target_link_libraries(min_capture_test PRIVATE ${VMA_HPP_TARGET_NAME})
target_link_libraries(min_capture_test2 PRIVATE ${VMA_HPP_TARGET_NAME})

# Link Vulkan and GLFW; on Windows also link the loader library explicitly.
if (WIN32)
    target_link_libraries(min_capture_test PRIVATE Vulkan::Vulkan glfw)
    target_link_libraries(min_capture_test2 PRIVATE Vulkan::Vulkan glfw)
else()
    target_link_libraries(min_capture_test PRIVATE Vulkan::Vulkan glfw)
    target_link_libraries(min_capture_test2 PRIVATE Vulkan::Vulkan glfw)
endif()

# Dispatcher macros (redundant with source file but safe)
target_compile_definitions(min_capture_test PRIVATE VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
target_compile_definitions(min_capture_test2 PRIVATE VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)

# Use C++20
target_compile_features(min_capture_test PRIVATE cxx_std_20)
target_compile_features(min_capture_test2 PRIVATE cxx_std_20)

# Enable MSVC warnings sensibly
if (MSVC)
    target_compile_options(min_capture_test PRIVATE /W4)
    target_compile_options(min_capture_test2 PRIVATE /W4)
else()
    target_compile_options(min_capture_test PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
    target_compile_options(min_capture_test2 PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
endif()
