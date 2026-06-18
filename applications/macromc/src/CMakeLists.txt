# MacroMC main application — tick-pacing-driven voxel host.
set(SOURCES
    main.cpp
    macromc_app.cpp
    macromc_app.h
    macromc_app_commands.cpp
    render_thread_context.cpp
    render_thread_context.h
    render_command.h
    gigavoxel_shell_registry.cpp
    gigavoxel_shell_registry.h
    gigavoxel_shell_handle.h
)

add_executable(macromc_app ${SOURCES})
mi_apply_app_manifest(macromc_app)

# MacroMC has its own shaders eventually; none for the skeleton yet.
# add_resources(macromc_app shaders "shaders/*.hlsl")

target_link_libraries(macromc_app PUBLIC
    infra core rhi rdg renderer util glfw
    macromc_registry macromc_world macromc_worldgen macromc_meshing
    macromc_public_headers
)

# Expose the bypass flag to the preprocessor (defined in parent CMakeLists).
# macromc_app reads MACROMC_BYPASS_RENDER_THREAD at runtime via this define.
if(MACROMC_BYPASS_RENDER_THREAD)
    target_compile_definitions(macromc_app PRIVATE MACROMC_BYPASS_RENDER_THREAD=1)
endif()

# MSVC parallel compilation PDB lock fix (consistent with other macromc libs).
if(MSVC)
    target_compile_options(macromc_app PRIVATE /FS /Z7)
endif()
