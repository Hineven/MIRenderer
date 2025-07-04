cmake_minimum_required(VERSION 3.28.0)

project(MI)

option(MI_USE_VCPKG_VMA_HPP "Use VMA-HPP from vcpkg" OFF)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 23)

# Extra CMake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Helper utilities
include(cmake/AddResources.cmake)

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    # Warning as error
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    # ...
else()
    message(FATAL_ERROR "Only MSVC and GNU compilers are supported.")
endif()

# Platform macros & checks
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    add_definitions(-DMI_COMPILER_MSVC)
endif()
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_definitions(-DMI_COMPILER_GNU)
    # Version > 13
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(FATAL_ERROR
                "GCC version < 14 is not supported."
                "Current version: ${CMAKE_CXX_COMPILER_VERSION}"
        )
    endif()
endif()

# For code to access project root (in debug builds)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions("-DMI_PROJECT_ROOT=\"${CMAKE_SOURCE_DIR}\"")
endif()

find_package(VulkanMemoryAllocator REQUIRED GLOBAL)
# Emit include directories for vma
if(NOT MI_USE_VCPKG_VMA_HPP)
    # Use submodule for VMA and VMA-HPP
    add_subdirectory(external/VulkanMemoryAllocator-Hpp)
    set(VMA_HPP_TARGET_NAME VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)
else()
    # Try to use VMA-HPP from vcpkg
    # Note: currently VMA-HPP on vcpkg is not up to date with the latest VMA version, causing compilation errors.
    find_package(unofficial-vulkan-memory-allocator-hpp REQUIRED GLOBAL)
    set(VMA_HPP_TARGET_NAME unofficial::VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)
endif()
# Emit found VMA version
message(STATUS "Found Vulkan Memory Allocator version: ${VulkanMemoryAllocator_VERSION}")
find_package(glm REQUIRED GLOBAL)

find_package(Vulkan REQUIRED GLOBAL COMPONENTS dxc GLOBAL)

# Emit found Vulkan SDK version
message(STATUS "Found Vulkan SDK version: ${Vulkan_VERSION}")

find_package(spirv_cross_core REQUIRED GLOBAL)
# spirv_cross_glsl is required for spirv_cross_hlsl
find_package(spirv_cross_glsl REQUIRED GLOBAL)
find_package(spirv_cross_hlsl REQUIRED GLOBAL)
find_package(spirv_cross_cpp REQUIRED GLOBAL)

find_package(SPIRV-Tools CONFIG REQUIRED)
find_package(SPIRV-Tools-opt CONFIG REQUIRED)
# this is buggy
#find_package(SPIRV-Tools-reduce CONFIG REQUIRED)

# GLFW for cross platform window creation and input handling
find_package(glfw3 CONFIG REQUIRED GLOBAL)

if(NOT Vulkan_dxc_lib_FOUND OR NOT Vulkan_dxc_DEBUG_LIBRARY)
    # Fallback to standalone DXC
    message(STATUS "No integrated dxc found within Vulkan SDK, using standalone DXC")
    if(NOT Vulkan_dxc_lib_FOUND)
        message(STATUS "Reason: dxc library not found")
    else()
        message(STATUS "Reason: dxc library found, but no debug library found.")
    endif()
    # Standalone DXC
    find_package(directx-dxc CONFIG REQUIRED GLOBAL)
    set(dxc_library_target Microsoft::DirectXShaderCompiler)
else()
    message(STATUS "Using integrated dxc within Vulkan SDK")
    set(dxc_library_target Vulkan::dxc_lib)
endif()

# Simple external packages
find_package(xxHash CONFIG REQUIRED)

# Simple implementation for the infrastracture abstraction within MI
# Doing environment specific stuff.
add_subdirectory(infra_impl)

# Core implementations
add_subdirectory(mi)

# tests
add_subdirectory(tests)

# application executables
add_subdirectory(applications)