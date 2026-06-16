# MacroMC Registry Module
cmake_minimum_required(VERSION 3.27)

add_library(macromc_registry STATIC
    src/block_registry.cpp
)

target_include_directories(macromc_registry PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(macromc_registry PUBLIC
    core
    macromc_public_headers
)
