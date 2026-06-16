# MacroMC Application
cmake_minimum_required(VERSION 3.27)

# Shared macromc helper aggregate header (re-exports common mi core helpers).
# INTERFACE library: just an include path, no compiled artefact.
add_library(macromc_public_headers INTERFACE)
target_include_directories(macromc_public_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Subdirectories
add_subdirectory(registry)
add_subdirectory(world)
add_subdirectory(worldgen)
add_subdirectory(meshing)

# Main application (placeholder for now)
# add_subdirectory(src)
