set(SOURCES
    3d_viewer.cpp
    viewer_app.cpp
    viewer_scene.cpp
    viewer_scene_loading.cpp
    viewer_console.cpp
    viewer_commands.cpp
    render_frame.cpp
    viewer_zmq.cpp
    viewer_control.cpp
    viewer_zmq.h
)

add_executable(3d_viewer ${SOURCES})


# ImGUI
find_package(imgui)
if (imgui_FOUND)
    target_link_libraries(3d_viewer PRIVATE imgui::imgui)
else()
    message(FATAL_ERROR "imgui not found")
endif()

# ZeroMQ (rcp with python)
find_package(ZeroMQ CONFIG REQUIRED)
find_package(cppzmq CONFIG REQUIRED)

# argparse
find_package(argparse CONFIG REQUIRED)

add_resources(3d_viewer shaders "shaders/*.hlsl")
add_resources(3d_viewer assets  "assets/*")
add_resources(3d_viewer scene_config "viewer_scene.json")

target_link_libraries(3d_viewer PUBLIC infra core rhi rdg renderer util glfw PRIVATE libzmq libzmq-static cppzmq cppzmq-static argparse::argparse)

# Copy NGX runtime DLLs to the output directory so DLSS features can be loaded
if(WIN32)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(NGX_DLL_DIR ${CMAKE_SOURCE_DIR}/external/DLSS/lib/Windows_x86_64/dev)
    else()
        set(NGX_DLL_DIR ${CMAKE_SOURCE_DIR}/external/DLSS/lib/Windows_x86_64/rel)
    endif()
    add_custom_command(TARGET 3d_viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${NGX_DLL_DIR}/nvngx_dlssd.dll
            ${NGX_DLL_DIR}/nvngx_dlss.dll
            ${NGX_DLL_DIR}/nvngx_dlssg.dll
            $<TARGET_FILE_DIR:3d_viewer>
        COMMENT "Copying NGX runtime DLLs to output directory"
    )
endif()
