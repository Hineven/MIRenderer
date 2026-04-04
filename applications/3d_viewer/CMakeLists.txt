set(SOURCES
    3d_viewer.cpp
    viewer_app.cpp
    viewer_scene.cpp
    viewer_scene_loading.cpp
    viewer_console.cpp
    viewer_commands.cpp
    render_frame.cpp
    viewer_export_channel.cpp
    viewer_zmq.cpp
    viewer_control.cpp
    viewer_export_channel.h
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
