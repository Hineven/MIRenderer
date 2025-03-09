cmake_minimum_required(VERSION 3.28.0)

project(MI)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 23)

# Our findvulkan fix
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    # Warning as error
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
else ()
    message(FATAL_ERROR "Unsupported compiler, only MSVC is supported (for now).")
endif()

# Platform macros
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    add_definitions(-DMI_COMPILER_MSVC)
endif()
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_definitions(-DMI_COMPILER_GCC)
endif()

# VMA-HPP from vcpkg
find_package(VulkanMemoryAllocator REQUIRED GLOBAL)
find_package(unofficial-vulkan-memory-allocator-hpp REQUIRED GLOBAL)
find_package(glm REQUIRED GLOBAL)

find_package(Vulkan REQUIRED COMPONENTS dxc GLOBAL)
find_package(spirv_cross_core REQUIRED GLOBAL)
# spirv_cross_glsl is required for spirv_cross_hlsl
find_package(spirv_cross_glsl REQUIRED GLOBAL)
find_package(spirv_cross_hlsl REQUIRED GLOBAL)
find_package(spirv_cross_cpp REQUIRED GLOBAL)

find_package(SPIRV-Tools CONFIG REQUIRED)
find_package(SPIRV-Tools-opt CONFIG REQUIRED)
# this is buggy
#find_package(SPIRV-Tools-reduce CONFIG REQUIRED)

if(NOT Vulkan_dxc_lib_FOUND OR NOT Vulkan_dxc_DEBUG_LIBRARY)
    # Fallback to standalone DXC
    message(STATUS "No integrated dxc found within Vulkan SDK, using standalone DXC")
    if(NOT ${Vulkan_dxc_DEBUG_LIBRARY})
        message(STATUS "Reason: dxc debug library not found")
    endif()
    # Standalone DXC
    find_package(directx-dxc CONFIG REQUIRED GLOBAL)
    set(dxc_library_target Microsoft::DirectXShaderCompiler)
else()
    message(STATUS "Using integrated dxc within Vulkan SDK")
    set(dxc_library_target Vulkan::dxc_lib)
endif()

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
target_link_libraries(hello core mi ${dxc_library_target} Vulkan::Vulkan spirv-cross-cpp spirv-cross-core spirv-cross-hlsl)

# tests
add_subdirectory(tests)