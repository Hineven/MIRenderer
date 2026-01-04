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

add_executable(min_rt_leak_case
    src/min_rt_leak_case.cpp
)

# Include dirs
target_include_directories(min_capture_test PRIVATE ${Vulkan_INCLUDE_DIRS})
target_include_directories(min_capture_test2 PRIVATE ${Vulkan_INCLUDE_DIRS})
target_include_directories(min_rt_leak_case PRIVATE ${Vulkan_INCLUDE_DIRS})

# VMA-Hpp (repo submodule or vcpkg, as configured by top-level)
# Top-level CMake defines VMA_HPP_TARGET_NAME;
target_link_libraries(min_capture_test PRIVATE ${VMA_HPP_TARGET_NAME})
target_link_libraries(min_capture_test2 PRIVATE ${VMA_HPP_TARGET_NAME})

# Link Vulkan and GLFW; on Windows also link the loader library explicitly.
if (WIN32)
    target_link_libraries(min_capture_test PRIVATE Vulkan::Vulkan glfw)
    target_link_libraries(min_capture_test2 PRIVATE Vulkan::Vulkan glfw)
    target_link_libraries(min_rt_leak_case PRIVATE Vulkan::Vulkan glfw)
else()
    target_link_libraries(min_capture_test PRIVATE Vulkan::Vulkan glfw)
    target_link_libraries(min_capture_test2 PRIVATE Vulkan::Vulkan glfw)
    target_link_libraries(min_rt_leak_case PRIVATE Vulkan::Vulkan glfw)
endif()

# Dispatcher macros (redundant with source file but safe)
target_compile_definitions(min_capture_test PRIVATE VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
target_compile_definitions(min_capture_test2 PRIVATE VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)
target_compile_definitions(min_rt_leak_case PRIVATE VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC=1 VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1)

# Use C++20
target_compile_features(min_capture_test PRIVATE cxx_std_20)
target_compile_features(min_capture_test2 PRIVATE cxx_std_20)
target_compile_features(min_rt_leak_case PRIVATE cxx_std_20)

# Enable MSVC warnings sensibly
if (MSVC)
    target_compile_options(min_capture_test PRIVATE /W4)
    target_compile_options(min_capture_test2 PRIVATE /W4)
    target_compile_options(min_rt_leak_case PRIVATE /W4)
else()
    target_compile_options(min_capture_test PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
    target_compile_options(min_capture_test2 PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
    target_compile_options(min_rt_leak_case PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
endif()

# Build-time GLSL -> SPIR-V using glslangValidator.
# The executable loads raygen.rgen.spv / miss.rmiss.spv from its working directory.
find_program(GLSLANG_VALIDATOR glslangValidator)
if (NOT GLSLANG_VALIDATOR)
    message(WARNING "glslangValidator not found; min_rt_leak_case will fail at runtime when loading SPIR-V. Install Vulkan SDK or provide glslangValidator in PATH.")
else()
    set(MIN_RT_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders/min_rt)
    # Generate SPIR-V into the build tree (safe in OUTPUT), then copy next to the executable.
    set(MIN_RT_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/min_rt_spv)
    file(MAKE_DIRECTORY ${MIN_RT_SPV_DIR})

    set(MIN_RT_RGEN_SPV ${MIN_RT_SPV_DIR}/raygen.rgen.spv)
    set(MIN_RT_RMISS_SPV ${MIN_RT_SPV_DIR}/miss.rmiss.spv)

    add_custom_command(
        OUTPUT ${MIN_RT_RGEN_SPV}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.2 ${MIN_RT_SHADER_DIR}/raygen.rgen -o ${MIN_RT_RGEN_SPV}
        DEPENDS ${MIN_RT_SHADER_DIR}/raygen.rgen
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${MIN_RT_RMISS_SPV}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.2 ${MIN_RT_SHADER_DIR}/miss.rmiss -o ${MIN_RT_RMISS_SPV}
        DEPENDS ${MIN_RT_SHADER_DIR}/miss.rmiss
        VERBATIM
    )

    add_custom_target(min_rt_leak_case_shaders ALL
        DEPENDS ${MIN_RT_RGEN_SPV} ${MIN_RT_RMISS_SPV}
    )
    add_dependencies(min_rt_leak_case min_rt_leak_case_shaders)

    add_custom_command(TARGET min_rt_leak_case POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MIN_RT_RGEN_SPV} $<TARGET_FILE_DIR:min_rt_leak_case>/raygen.rgen.spv
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MIN_RT_RMISS_SPV} $<TARGET_FILE_DIR:min_rt_leak_case>/miss.rmiss.spv
        VERBATIM
    )
endif()
