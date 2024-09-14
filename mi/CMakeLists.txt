cmake_minimum_required(VERSION 3.26.0)

project(MIRenderer)

enable_language(CXX)
set(CMAKE_CXX_STANDARD 20)

message(STATUS "CMAKE_CXX_COMPILER_ID: ${CMAKE_CXX_COMPILER_ID}")
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    # Warning as error
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /WX /W4")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    # Warning as error
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror -Wall -Wextra -Wpedantic")
else()
    message(FATAL_ERROR "Only MSVC and GNU G++ are supported. Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()


# Platform macros
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
    add_definitions(-DMI_COMPILER_MSVC)
endif()
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    add_definitions(-DMI_COMPILER_GCC)
endif()

##############################################
#                  Dependencies              #
##############################################
find_package(Vulkan REQUIRED COMPONENTS dxc SPIRV-Tools)
if(NOT Vulkan_dxc_DEBUG_LIBRARY)
    # The script shipped with CMake has failed to find the debug version of dxcompiler
    # Use a bug-fixed version of the script to try twice.
    message(STATUS "Failed to find Vulkan_dxc_DEBUG_LIBRARY, resorting to our version of FindVulkan.cmake...")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(Vulkan REQUIRED COMPONENTS dxc SPIRV-Tools)
    if(NOT Vulkan_dxc_DEBUG_LIBRARY)
        message(FATAL_ERROR "Failed to find VulkanFixed_dxc_DEBUG_LIBRARY with final resort, please check your Vulkan SDK installation.")
    else()
        message(STATUS "Found VulkanFixed_dxc_DEBUG_LIBRARY: ${Vulkan_dxc_DEBUG_LIBRARY}")
    endif()
endif()

find_package(VulkanMemoryAllocator REQUIRED COMPONENTS VulkanMemoryAllocator)

find_package(glm REQUIRED)

# core library
add_subdirectory(core)
# utilities
add_subdirectory(util)
# render hardware abstraction
add_subdirectory(rhi)
# top layer renderer logic
add_subdirectory(ml)
