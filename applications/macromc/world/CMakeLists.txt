# MacroMC World Module
cmake_minimum_required(VERSION 3.27)

add_library(macromc_world STATIC
    src/block_data.cpp
    src/chunk_data.cpp
    src/shell_data.cpp
    src/world_data.cpp
    src/worldgen.cpp
)

target_include_directories(macromc_world PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(macromc_world PUBLIC
    core
    macromc_registry
    glm::glm
)
