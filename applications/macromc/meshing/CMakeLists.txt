# macromc_meshing: CPU-side greedy meshing + multi-threaded meshing scheduler.
# Pure host computation (task system + chunk data); no RHI/renderer dependency.
cmake_minimum_required(VERSION 3.27)

add_library(macromc_meshing STATIC
    src/greedy_mesher.cpp
    src/chunk_meshing_context.cpp
)

target_include_directories(macromc_meshing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(macromc_meshing PUBLIC
    core
    macromc_registry
    macromc_world
    macromc_public_headers
    glm::glm
)

if(MSVC)
    target_compile_options(macromc_meshing PRIVATE /FS /Z7)
endif()
