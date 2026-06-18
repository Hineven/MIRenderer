# MacroMC Application
cmake_minimum_required(VERSION 3.27)

# Shared macromc helper aggregate header (re-exports common mi core helpers).
# INTERFACE library: just an include path, no compiled artefact.
add_library(macromc_public_headers INTERFACE)
target_include_directories(macromc_public_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Gameplay/render thread split toggle (THREAD_MODEL.md). ON = single-threaded
# (debug-friendly; all RHI asserts see one thread). OFF = real three-thread
# split (main/gameplay/render). Default ON so the skeleton is exercised in its
# safest mode first; flip to OFF once the render thread path is validated.
option(MACROMC_BYPASS_RENDER_THREAD "Run MacroMC single-threaded (merge gameplay/render)" ON)
if(MACROMC_BYPASS_RENDER_THREAD)
    set(MACROMC_BYPASS_RENDER_THREAD_DEFINE MACROMC_BYPASS_RENDER_THREAD=1)
    message(STATUS "MacroMC: render thread BYPASSED (single-threaded debug mode)")
else()
    message(STATUS "MacroMC: gameplay/render thread split ENABLED")
endif()

# Subdirectories
# profiler is the lowest macromc module (depends only on core); every other
# module links it, so it must be declared first.
add_subdirectory(profiler)
add_subdirectory(registry)
add_subdirectory(world)
add_subdirectory(worldgen)
add_subdirectory(meshing)

# Main application
add_subdirectory(src)
