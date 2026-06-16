# MacroMC Worldgen Module
cmake_minimum_required(VERSION 3.27)

add_library(macromc_worldgen STATIC
    src/perlin_noise.cpp
    src/simple_terrain_worldgen.cpp
    src/empty_worldgen.cpp
)

target_include_directories(macromc_worldgen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(macromc_worldgen PUBLIC
    core
    macromc_world
    macromc_registry
    macromc_public_headers
    glm::glm
)

# Fix MSVC PDB lock
if(MSVC)
    target_compile_options(macromc_worldgen PRIVATE /FS /Z7)
endif()
