cmake_minimum_required(VERSION 3.28.0)

project(MI)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)

# Building options
option(MI_BUILD_RHI_D3D12 "Build a RHI with D3D12 backend" ON)
option(MI_BUILD_RHI_VULKAN "Build a RHI with Vulkan backend" OFF)

# Only one backend can be enabled
if(MI_BUILD_RHI_D3D12 AND MI_BUILD_RHI_VULKAN)
    message(FATAL_ERROR "Only one RHI backend can be enabled")
endif()

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    # Warning as error
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    # Macros
    add_definitions(-DMI_COMPILER_MSVC)
else ()
    message(FATAL_ERROR "Unsupported compiler, only MSVC is supported (for now).")
endif()

find_package(glm REQUIRED GLOBAL)
find_package(spirv_cross_core REQUIRED GLOBAL)
# spirv_cross_glsl is required for spirv_cross_hlsl
find_package(spirv_cross_glsl REQUIRED GLOBAL)
find_package(spirv_cross_hlsl REQUIRED GLOBAL)
find_package(spirv_cross_cpp REQUIRED GLOBAL)

find_package(SPIRV-Tools CONFIG REQUIRED)
find_package(SPIRV-Tools-opt CONFIG REQUIRED)

set(EXTERNAL_LIBRARIES spirv-cross-cpp spirv-cross-core spirv-cross-hlsl)

# Simple implementation for the infrastracture abstraction within MI
# Doing environment specific stuff.
add_subdirectory(infra_impl)

# Core implementations
add_subdirectory(mi)

# Samples

add_executable(
    hello
    samples/hello/main.cpp
)

target_link_libraries(hello core mi ${EXTERNAL_LIBRARIES})

# tests
add_subdirectory(tests)