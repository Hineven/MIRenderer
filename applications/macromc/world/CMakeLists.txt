# MacroMC World Module
cmake_minimum_required(VERSION 3.27)

add_library(macromc_world STATIC
    src/block_data.cpp
    src/palette.cpp
    src/sub_chunk.cpp
    src/chunk_data.cpp
    src/shell_data.cpp
    src/world_data.cpp
)

target_include_directories(macromc_world PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(macromc_world PUBLIC
    core
    macromc_registry
    glm::glm
)

# Fix MSVC parallel compilation PDB lock
if(MSVC)
    target_compile_options(macromc_world PRIVATE /FS /Z7)
endif()
