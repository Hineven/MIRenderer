cmake_minimum_required(VERSION 3.26.0)

project(MI)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    # Warning as error
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /WX /W4")
else ()
    message(FATAL_ERROR "Unsupported compiler, only MSVC is supported (for now).")
endif()

# Simple implementation for the infrastracture abstraction within MI
# Doing environment specific stuff.
add_subdirectory(infra_impl)

# Core implementations
add_subdirectory(mi)

find_package(Vulkan REQUIRED)
find_package(spirv_cross_core REQUIRED)
# spirv_cross_glsl is required for spirv_cross_hlsl
find_package(spirv_cross_glsl REQUIRED)
find_package(spirv_cross_hlsl REQUIRED)
find_package(spirv_cross_cpp REQUIRED)

if(NOT ${Vulkan_dxc_FOUND})
    # Fallback to standalone DXC
    message(STATUS "No integrated dxc found within Vulkan SDK, using standalone DXC")
    find_package(directx-dxc CONFIG REQUIRED)
    set(dxc_library_target Microsoft::DirectXShaderCompiler)
else()
    set(dxc_library_target Vulkan::dxc_lib)
endif()

# Samples

add_executable(
    hello
    samples/hello/main.cpp
)
target_link_libraries(hello core mi ${dxc_library_target} Vulkan::Vulkan spirv-cross-cpp spirv-cross-core spirv-cross-hlsl)