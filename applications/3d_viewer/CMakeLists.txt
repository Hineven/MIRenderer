set(SOURCES
    3d_viewer.cpp
    viewer_app.cpp
    viewer_console.cpp
    render_frame.cpp
    viewer_zmq.cpp
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

add_resources(3d_viewer shaders "shaders/*.hlsl")
add_resources(3d_viewer assets  "assets/*")

target_link_libraries(3d_viewer PUBLIC infra core rhi rdg renderer util glfw PRIVATE libzmq libzmq-static cppzmq cppzmq-static)